#include "swiftkv/persistence.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "swiftkv/protocol.hpp"

using namespace swiftkv;
namespace fs = std::filesystem;

namespace {

/// A log file in a unique temporary directory, removed on scope exit.
struct TempLog {
    fs::path dir;
    fs::path path;

    TempLog() {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path() /
              ("swiftkv-test-" + std::to_string(::getpid()) + "-" +
               std::to_string(counter.fetch_add(1)));
        fs::create_directories(dir);
        path = dir / "appendonly.aof";
    }

    ~TempLog() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    [[nodiscard]] AppendOnlyLog::Config config(
        AppendOnlyLog::SyncPolicy sync = AppendOnlyLog::SyncPolicy::Always) const {
        AppendOnlyLog::Config config;
        config.path = path.string();
        config.sync = sync;
        return config;
    }

    void write_raw(const std::string& bytes) const {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] std::string read_raw() const {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

    [[nodiscard]] std::uintmax_t size() const {
        std::error_code ec;
        const auto n = fs::file_size(path, ec);
        return ec ? 0 : n;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST_CASE("writes survive a restart", "[persistence]") {
    TempLog temp;

    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "a", "1"});
        log.append({"SET", "b", "2"});
        log.close();
    }

    Store store;
    AppendOnlyLog log(temp.config());
    const auto result = log.replay(store);

    REQUIRE(result.ok);
    REQUIRE(result.commands_applied == 2);
    REQUIRE(store.get("a") == "1");
    REQUIRE(store.get("b") == "2");
}

TEST_CASE("a missing log is a normal first start", "[persistence]") {
    TempLog temp;
    Store store;
    AppendOnlyLog log(temp.config());

    const auto result = log.replay(store);

    REQUIRE(result.ok);
    REQUIRE(result.commands_applied == 0);
    REQUIRE(store.empty());
}

TEST_CASE("replay applies the last write to a key", "[persistence]") {
    TempLog temp;
    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "k", "first"});
        log.append({"SET", "k", "second"});
        log.append({"SET", "k", "third"});
        log.close();
    }

    Store store;
    AppendOnlyLog log(temp.config());
    log.replay(store);

    REQUIRE(store.get("k") == "third");
}

TEST_CASE("deletes are replayed", "[persistence]") {
    TempLog temp;
    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "a", "1"});
        log.append({"SET", "b", "2"});
        log.append({"DEL", "a"});
        log.close();
    }

    Store store;
    AppendOnlyLog log(temp.config());
    log.replay(store);

    REQUIRE_FALSE(store.contains("a"));
    REQUIRE(store.contains("b"));
}

TEST_CASE("FLUSHALL is replayed", "[persistence]") {
    TempLog temp;
    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "a", "1"});
        log.append({"FLUSHALL"});
        log.append({"SET", "b", "2"});
        log.close();
    }

    Store store;
    AppendOnlyLog log(temp.config());
    log.replay(store);

    REQUIRE_FALSE(store.contains("a"));
    REQUIRE(store.contains("b"));
}

TEST_CASE("binary keys and values survive a round trip", "[persistence]") {
    TempLog temp;
    const std::string key = std::string("k\0\r\ney", 6);
    // The escape is closed before "al", otherwise \xffal reads as one
    // out-of-range hex escape rather than 0xff followed by letters.
    const std::string value = std::string("v\0\r\n\xff" "al", 7);

    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", key, value});
        log.close();
    }

    Store store;
    AppendOnlyLog log(temp.config());
    log.replay(store);

    REQUIRE(store.get(key) == value);
}

TEST_CASE("large values survive a round trip", "[persistence]") {
    TempLog temp;
    const std::string value(4 * 1024 * 1024, 'x');  // crosses the replay chunk

    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "big", value});
        log.close();
    }

    Store store;
    AppendOnlyLog log(temp.config());
    const auto result = log.replay(store);

    REQUIRE(result.ok);
    REQUIRE(store.get("big") == value);
}

// ---------------------------------------------------------------------------
// Crash recovery
// ---------------------------------------------------------------------------

TEST_CASE("a partial trailing record is discarded, not fatal", "[persistence][crash]") {
    // What a crash mid-write actually leaves behind. The client never received
    // an acknowledgement for that command, so dropping it loses nothing that
    // was ever promised.
    TempLog temp;
    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "good", "value"});
        log.close();
    }
    temp.write_raw("*3\r\n$3\r\nSET\r\n$4\r\nhalf");  // truncated

    Store store;
    AppendOnlyLog log(temp.config());
    const auto result = log.replay(store);

    REQUIRE(result.ok);
    REQUIRE(result.commands_applied == 1);
    REQUIRE(result.bytes_discarded > 0);
    REQUIRE(store.get("good") == "value");
    REQUIRE_FALSE(store.contains("half"));
}

TEST_CASE("truncation at every offset is survivable", "[persistence][crash]") {
    // A crash can land anywhere, so recovery must hold for every possible
    // truncation point -- not just a convenient one.
    TempLog temp;
    {
        AppendOnlyLog log(temp.config());
        REQUIRE(log.open().ok);
        log.append({"SET", "a", "1"});
        log.append({"SET", "b", "2"});
        log.append({"SET", "c", "3"});
        log.close();
    }
    const std::string whole = temp.read_raw();

    for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
        TempLog partial;
        partial.write_raw(whole.substr(0, cut));

        Store store;
        AppendOnlyLog log(partial.config());
        const auto result = log.replay(store);

        INFO("truncated at byte " << cut << " of " << whole.size());
        REQUIRE(result.ok);
        // Whatever survived must be a prefix of the original writes.
        if (store.contains("c")) {
            REQUIRE(store.contains("b"));
        }
        if (store.contains("b")) {
            REQUIRE(store.contains("a"));
        }
    }
}

TEST_CASE("genuinely corrupt content is reported, not silently ignored",
          "[persistence][crash]") {
    // Distinct from a truncated tail: garbage in the middle means something is
    // wrong that recovery should not paper over.
    TempLog temp;
    temp.write_raw("this is not a RESP record at all\r\n");

    Store store;
    AppendOnlyLog log(temp.config());
    const auto result = log.replay(store);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.find("corrupt") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Rewrite (compaction)
// ---------------------------------------------------------------------------

TEST_CASE("rewrite compacts repeated writes to one record per key",
          "[persistence][rewrite]") {
    // The log records history, not state: a key written a thousand times costs
    // a thousand records until it is rewritten.
    TempLog temp;
    AppendOnlyLog log(temp.config());
    REQUIRE(log.open().ok);

    Store store;
    for (int i = 0; i < 1000; ++i) {
        log.append({"SET", "hot", "value" + std::to_string(i)});
        store.set("hot", "value" + std::to_string(i));
    }
    const auto before = temp.size();

    REQUIRE(log.rewrite(store).ok);
    const auto after = temp.size();
    log.close();

    REQUIRE(after < before / 10);

    Store restored;
    AppendOnlyLog reader(temp.config());
    reader.replay(restored);
    REQUIRE(restored.get("hot") == "value999");
    REQUIRE(restored.size() == 1);
}

TEST_CASE("rewrite preserves every key", "[persistence][rewrite]") {
    TempLog temp;
    AppendOnlyLog log(temp.config());
    REQUIRE(log.open().ok);

    Store store;
    for (int i = 0; i < 500; ++i) {
        const std::string key = "k" + std::to_string(i);
        store.set(key, "v" + std::to_string(i));
        log.append({"SET", key, "v" + std::to_string(i)});
    }

    REQUIRE(log.rewrite(store).ok);
    log.close();

    Store restored;
    AppendOnlyLog reader(temp.config());
    reader.replay(restored);

    REQUIRE(restored.size() == 500);
    for (int i = 0; i < 500; ++i) {
        REQUIRE(restored.get("k" + std::to_string(i)) == "v" + std::to_string(i));
    }
}

TEST_CASE("appends after a rewrite still land in the log", "[persistence][rewrite]") {
    // The old descriptor points at the replaced inode, so the log must be
    // reopened -- otherwise later writes vanish silently.
    TempLog temp;
    AppendOnlyLog log(temp.config());
    REQUIRE(log.open().ok);

    Store store;
    store.set("a", "1");
    log.append({"SET", "a", "1"});
    REQUIRE(log.rewrite(store).ok);

    log.append({"SET", "after", "rewrite"});
    log.close();

    Store restored;
    AppendOnlyLog reader(temp.config());
    reader.replay(restored);

    REQUIRE(restored.get("a") == "1");
    REQUIRE(restored.get("after") == "rewrite");
}

TEST_CASE("rewrite leaves no temporary file behind", "[persistence][rewrite]") {
    TempLog temp;
    AppendOnlyLog log(temp.config());
    REQUIRE(log.open().ok);
    Store store;
    store.set("a", "1");

    REQUIRE(log.rewrite(store).ok);
    log.close();

    REQUIRE_FALSE(fs::exists(temp.path.string() + ".rewrite"));
}

// ---------------------------------------------------------------------------
// Policies and stats
// ---------------------------------------------------------------------------

TEST_CASE("every sync policy round trips", "[persistence]") {
    for (auto policy : {AppendOnlyLog::SyncPolicy::Always,
                        AppendOnlyLog::SyncPolicy::EverySecond,
                        AppendOnlyLog::SyncPolicy::Never}) {
        TempLog temp;
        {
            AppendOnlyLog log(temp.config(policy));
            REQUIRE(log.open().ok);
            log.append({"SET", "k", "v"});
            log.close();  // close always flushes and fsyncs
        }

        Store store;
        AppendOnlyLog reader(temp.config(policy));
        reader.replay(store);

        INFO("policy: " << to_string(policy));
        REQUIRE(store.get("k") == "v");
    }
}

TEST_CASE("sync policy names parse both ways", "[persistence]") {
    AppendOnlyLog::SyncPolicy policy{};

    REQUIRE(parse_sync_policy("always", policy));
    REQUIRE(policy == AppendOnlyLog::SyncPolicy::Always);
    REQUIRE(parse_sync_policy("everysec", policy));
    REQUIRE(policy == AppendOnlyLog::SyncPolicy::EverySecond);
    REQUIRE(parse_sync_policy("never", policy));
    REQUIRE(policy == AppendOnlyLog::SyncPolicy::Never);
    REQUIRE_FALSE(parse_sync_policy("sometimes", policy));

    REQUIRE(to_string(AppendOnlyLog::SyncPolicy::EverySecond) == "everysec");
}

TEST_CASE("an empty path is rejected", "[persistence]") {
    AppendOnlyLog::Config config;
    AppendOnlyLog log(config);

    REQUIRE_FALSE(log.open().ok);
}

TEST_CASE("stats count records and bytes", "[persistence]") {
    TempLog temp;
    AppendOnlyLog log(temp.config());
    REQUIRE(log.open().ok);

    log.append({"SET", "a", "1"});
    log.append({"SET", "b", "2"});
    log.sync();

    const auto stats = log.stats();
    REQUIRE(stats.records_appended == 2);
    REQUIRE(stats.bytes_appended > 0);
    REQUIRE(log.size_on_disk() > 0);
}

// ---------------------------------------------------------------------------
// Concurrency -- many event loops append to one log
// ---------------------------------------------------------------------------

TEST_CASE("concurrent appends produce a readable log", "[persistence][concurrency]") {
    // Interleaved writes must never split a record; a torn record would make
    // everything after it unreadable.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    TempLog temp;
    {
        AppendOnlyLog log(temp.config(AppendOnlyLog::SyncPolicy::Never));
        REQUIRE(log.open().ok);

        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&log, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    log.append({"SET", "t" + std::to_string(t) + ":k" + std::to_string(i),
                                "v" + std::to_string(i)});
                }
            });
        }
        threads.clear();
        log.close();
    }

    Store store;
    AppendOnlyLog reader(temp.config());
    const auto result = reader.replay(store);

    REQUIRE(result.ok);
    REQUIRE(result.commands_applied == kThreads * kPerThread);
    REQUIRE(store.size() == kThreads * kPerThread);
}
