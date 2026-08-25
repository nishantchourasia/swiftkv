#include "swiftkv/server.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "swiftkv/client.hpp"
#include "swiftkv/persistence.hpp"

using namespace swiftkv;
using namespace std::chrono_literals;

namespace {

/// Starts a real server on a kernel-assigned port and stops it on scope exit.
///
/// Port 0 is used throughout: the kernel picks a free port, so these tests
/// never collide with another process on this shared machine, and several test
/// binaries can run in parallel.
struct TestServer {
    Server server;

    explicit TestServer(Server::Config config = {}) : server(prepare(std::move(config))) {
        const auto result = server.start();
        REQUIRE(result.ok);
        REQUIRE(server.port() != 0);
    }

    ~TestServer() { server.stop(); }

    [[nodiscard]] std::uint16_t port() const { return server.port(); }

    static Server::Config prepare(Server::Config config) {
        config.host = "127.0.0.1";
        config.port = 0;
        if (config.io_threads == 0) {
            config.io_threads = 2;  // deterministic, and enough to exercise handoff
        }
        return config;
    }

    Client connect() const {
        Client client;
        const auto result = client.connect("127.0.0.1", port(), 5s);
        REQUIRE(result.ok);
        return client;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("server starts and reports its port", "[server]") {
    TestServer fixture;

    REQUIRE(fixture.server.running());
    REQUIRE(fixture.port() > 0);
}

TEST_CASE("stop is idempotent", "[server]") {
    TestServer fixture;

    fixture.server.stop();
    fixture.server.stop();

    REQUIRE_FALSE(fixture.server.running());
}

TEST_CASE("binding an unavailable address fails cleanly", "[server]") {
    Server::Config config;
    config.host = "203.0.113.1";  // TEST-NET-3, not assigned to this host
    config.port = 0;
    Server server(config);

    const auto result = server.start();

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(server.running());
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST_CASE("PING round trips", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();

    REQUIRE(client.ping());
}

TEST_CASE("SET then GET returns the value", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();

    REQUIRE(client.set("foo", "bar"));
    REQUIRE(client.get("foo") == "bar");
}

TEST_CASE("GET of a missing key returns null", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();

    auto reply = client.command({"GET", "absent"});

    REQUIRE(reply);
    REQUIRE(reply->is_null());
}

TEST_CASE("DEL reports how many keys were removed", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();
    client.set("a", "1");

    REQUIRE(client.del("a") == 1);
    REQUIRE(client.del("a") == 0);
}

TEST_CASE("values survive a large round trip", "[server]") {
    // Crosses the 64 KB read chunk, so the server must reassemble across reads.
    TestServer fixture;
    auto client = fixture.connect();
    const std::string value(512 * 1024, 'z');

    REQUIRE(client.set("big", value));
    REQUIRE(client.get("big") == value);
}

TEST_CASE("binary values survive a round trip", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();
    const std::string value = std::string("bin\0\r\n\xff data", 12);

    REQUIRE(client.set("k", value));
    REQUIRE(client.get("k") == value);
}

TEST_CASE("state is shared between connections", "[server]") {
    TestServer fixture;
    auto writer = fixture.connect();
    auto reader = fixture.connect();

    writer.set("shared", "value");

    REQUIRE(reader.get("shared") == "value");
}

// ---------------------------------------------------------------------------
// Pipelining
// ---------------------------------------------------------------------------

TEST_CASE("pipelined commands are answered in order", "[server][pipeline]") {
    // A busy client sends many commands without waiting. One read on the server
    // may therefore carry several, and the replies must come back in order.
    TestServer fixture;
    auto client = fixture.connect();

    std::string batch;
    for (int i = 0; i < 100; ++i) {
        batch += encode_array({"SET", "k" + std::to_string(i), "v" + std::to_string(i)});
    }
    for (int i = 0; i < 100; ++i) {
        batch += encode_array({"GET", "k" + std::to_string(i)});
    }
    REQUIRE(client.send_raw(batch).ok);

    for (int i = 0; i < 100; ++i) {
        auto reply = client.read_reply();
        REQUIRE(reply);
        REQUIRE(reply->text == "OK");
    }
    for (int i = 0; i < 100; ++i) {
        auto reply = client.read_reply();
        REQUIRE(reply);
        REQUIRE(reply->text == "v" + std::to_string(i));
    }
}

TEST_CASE("a command split across packets is reassembled", "[server][pipeline]") {
    // TCP does not preserve message boundaries; the server must not assume one
    // read equals one command.
    TestServer fixture;
    auto client = fixture.connect();

    const std::string request = encode_array({"SET", "split", "value"});
    for (char c : request) {
        REQUIRE(client.send_raw(std::string_view(&c, 1)).ok);
        std::this_thread::sleep_for(1ms);
    }

    auto reply = client.read_reply();
    REQUIRE(reply);
    REQUIRE(reply->text == "OK");
    REQUIRE(client.get("split") == "value");
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST_CASE("many concurrent clients are served correctly", "[server][concurrency]") {
    constexpr int kClients = 32;
    constexpr int kOpsPerClient = 200;

    Server::Config config;
    config.io_threads = 4;
    config.store.max_entries_per_shard = 100'000;
    TestServer fixture(config);

    std::atomic<int> failures{0};
    {
        std::vector<std::jthread> threads;
        for (int c = 0; c < kClients; ++c) {
            threads.emplace_back([&, c] {
                Client client;
                if (!client.connect("127.0.0.1", fixture.port(), 10s).ok) {
                    failures.fetch_add(1);
                    return;
                }
                for (int i = 0; i < kOpsPerClient; ++i) {
                    const std::string key = "c" + std::to_string(c) + ":k" + std::to_string(i);
                    const std::string value = "v" + std::to_string(c * 1000 + i);
                    if (!client.set(key, value) || client.get(key) != value) {
                        failures.fetch_add(1);
                        return;
                    }
                }
            });
        }
    }

    REQUIRE(failures.load() == 0);
    REQUIRE(fixture.server.store().size() == kClients * kOpsPerClient);
}

TEST_CASE("connections are distributed across event loops", "[server][concurrency]") {
    Server::Config config;
    config.io_threads = 4;
    TestServer fixture(config);

    std::vector<Client> clients;
    for (int i = 0; i < 16; ++i) {
        clients.push_back(fixture.connect());
        REQUIRE(clients.back().ping());
    }

    REQUIRE(fixture.server.metrics().connections_accepted.load() == 16);
    REQUIRE(fixture.server.metrics().connections_current.load() == 16);
}

TEST_CASE("closing a connection releases it", "[server][concurrency]") {
    TestServer fixture;

    {
        auto client = fixture.connect();
        REQUIRE(client.ping());
        REQUIRE(fixture.server.metrics().connections_current.load() == 1);
    }

    // The server notices asynchronously, so poll rather than assuming timing.
    for (int i = 0; i < 200 && fixture.server.metrics().connections_current.load() != 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(fixture.server.metrics().connections_current.load() == 0);
}

// ---------------------------------------------------------------------------
// Robustness and limits
// ---------------------------------------------------------------------------

TEST_CASE("malformed input is refused and the connection closed", "[server][security]") {
    // Once the byte stream is out of sync there is no way to resynchronise, so
    // the server must not try to guess where the next command starts.
    TestServer fixture;
    auto client = fixture.connect();

    REQUIRE(client.send_raw("this is not RESP\r\n").ok);

    auto reply = client.read_reply();
    REQUIRE(reply);
    REQUIRE(reply->is_error());
    REQUIRE(reply->text.find("protocol error") != std::string::npos);
}

TEST_CASE("an oversized declared length is refused", "[server][security]") {
    TestServer fixture;
    auto client = fixture.connect();

    // 4 GB announced in a handful of bytes.
    REQUIRE(client.send_raw("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4294967295\r\n").ok);

    auto reply = client.read_reply();
    REQUIRE(reply);
    REQUIRE(reply->is_error());
}

TEST_CASE("malformed input from one client does not affect others", "[server][security]") {
    TestServer fixture;
    auto victim = fixture.connect();
    victim.set("survives", "yes");

    {
        auto attacker = fixture.connect();
        REQUIRE(attacker.send_raw("garbage\r\n").ok);
        attacker.read_reply();
    }

    REQUIRE(victim.get("survives") == "yes");
    REQUIRE(victim.ping());
}

TEST_CASE("the connection limit is enforced", "[server][security]") {
    Server::Config config;
    config.max_connections = 4;
    config.io_threads = 1;
    TestServer fixture(config);

    std::vector<Client> accepted;
    for (int i = 0; i < 4; ++i) {
        accepted.push_back(fixture.connect());
        REQUIRE(accepted.back().ping());
    }

    // The server accepts the socket, replies with an error and closes, so the
    // client learns at once rather than hanging in the backlog.
    Client extra;
    REQUIRE(extra.connect("127.0.0.1", fixture.port(), 5s).ok);
    auto reply = extra.read_reply();

    REQUIRE(reply);
    REQUIRE(reply->is_error());
    REQUIRE(reply->text.find("max number of clients") != std::string::npos);
    REQUIRE(fixture.server.metrics().connections_rejected.load() >= 1);
}

TEST_CASE("QUIT closes the connection after replying", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();

    auto reply = client.command({"QUIT"});

    REQUIRE(reply);
    REQUIRE(reply->text == "OK");
    // The server closes, so the next read finds end-of-stream.
    REQUIRE_FALSE(client.read_reply().has_value());
}

TEST_CASE("an unknown command is an error, not a disconnect", "[server]") {
    TestServer fixture;
    auto client = fixture.connect();

    auto reply = client.command({"NOSUCHCOMMAND"});

    REQUIRE(reply);
    REQUIRE(reply->is_error());
    REQUIRE(client.ping());  // connection still usable
}

TEST_CASE("idle connections are reaped", "[server][reliability]") {
    // A client that vanishes without closing would otherwise hold its
    // descriptor until the process restarts.
    Server::Config config;
    config.idle_timeout = 1s;
    config.io_threads = 1;
    TestServer fixture(config);

    auto client = fixture.connect();
    REQUIRE(client.ping());
    REQUIRE(fixture.server.metrics().connections_current.load() == 1);

    for (int i = 0; i < 400 && fixture.server.metrics().connections_current.load() != 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }

    REQUIRE(fixture.server.metrics().connections_current.load() == 0);
}

// ---------------------------------------------------------------------------
// Observability
// ---------------------------------------------------------------------------

TEST_CASE("INFO reflects real traffic", "[server][metrics]") {
    TestServer fixture;
    auto client = fixture.connect();
    client.set("a", "1");
    client.get("a");
    client.get("missing");

    const std::string info = fixture.server.info();

    REQUIRE(info.find("keys:1") != std::string::npos);
    REQUIRE(info.find("keyspace_hits:1") != std::string::npos);
    REQUIRE(info.find("keyspace_misses:1") != std::string::npos);
    REQUIRE(info.find("connected_clients:1") != std::string::npos);
}

TEST_CASE("byte counters move", "[server][metrics]") {
    TestServer fixture;
    auto client = fixture.connect();
    client.set("k", "v");

    REQUIRE(fixture.server.metrics().bytes_read.load() > 0);
    REQUIRE(fixture.server.metrics().bytes_written.load() > 0);
}

TEST_CASE("metrics endpoint is Prometheus-shaped", "[server][metrics]") {
    TestServer fixture;
    auto client = fixture.connect();
    client.set("k", "v");

    const std::string text = fixture.server.metrics_text();

    REQUIRE(text.find("swiftkv_sets_total 1") != std::string::npos);
    REQUIRE(text.find("swiftkv_connections_accepted_total 1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Durability -- data must survive a restart
// ---------------------------------------------------------------------------

namespace {

/// A unique temporary directory for a log file, cleaned up on scope exit.
struct TempDir {
    std::string path;

    TempDir() {
        static std::atomic<int> counter{0};
        path = "/tmp/swiftkv-srv-" + std::to_string(::getpid()) + "-" +
               std::to_string(counter.fetch_add(1));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::string aof() const { return path + "/appendonly.aof"; }
};

}  // namespace

TEST_CASE("data survives a clean restart", "[server][durability]") {
    TempDir dir;

    {
        Server::Config config;
        config.aof_path = dir.aof();
        config.aof_sync = AppendOnlyLog::SyncPolicy::Always;
        TestServer fixture(config);
        auto client = fixture.connect();

        REQUIRE(client.set("persisted", "value"));
        REQUIRE(client.set("deleted", "gone"));
        REQUIRE(client.del("deleted") == 1);
    }

    Server::Config config;
    config.aof_path = dir.aof();
    TestServer fixture(config);
    auto client = fixture.connect();

    REQUIRE(client.get("persisted") == "value");
    REQUIRE_FALSE(client.command({"GET", "deleted"})->text == "gone");
    REQUIRE(fixture.server.replay_result().commands_applied == 3);
}

TEST_CASE("without a log path nothing is persisted", "[server][durability]") {
    // The default is a pure cache: no file is written at all.
    TempDir dir;
    {
        TestServer fixture;  // no aof_path
        auto client = fixture.connect();
        REQUIRE(client.set("k", "v"));
        REQUIRE(fixture.server.log() == nullptr);
    }
    REQUIRE_FALSE(std::filesystem::exists(dir.aof()));
}

TEST_CASE("a corrupt log stops the server from starting", "[server][durability]") {
    // Refusing is safer than serving a silently incomplete dataset; an operator
    // can then inspect or move the file deliberately.
    TempDir dir;
    {
        std::ofstream out(dir.aof(), std::ios::binary);
        out << "this is not a valid RESP record\r\n";
    }

    Server::Config config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.aof_path = dir.aof();
    Server server(config);

    const auto result = server.start();

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.find("could not be replayed") != std::string::npos);
}

TEST_CASE("reads are not written to the log", "[server][durability]") {
    // Logging GETs would multiply the log's size by the read ratio and replay
    // would be unaffected by them anyway.
    TempDir dir;
    Server::Config config;
    config.aof_path = dir.aof();
    config.aof_sync = AppendOnlyLog::SyncPolicy::Always;
    TestServer fixture(config);
    auto client = fixture.connect();

    client.set("k", "v");
    const auto after_write = fixture.server.log()->stats().records_appended;
    for (int i = 0; i < 100; ++i) {
        client.get("k");
    }

    REQUIRE(fixture.server.log()->stats().records_appended == after_write);
}

TEST_CASE("concurrent writes are all durable", "[server][durability]") {
    TempDir dir;
    constexpr int kClients = 8;
    constexpr int kPerClient = 200;

    {
        Server::Config config;
        config.aof_path = dir.aof();
        config.io_threads = 4;
        TestServer fixture(config);

        std::vector<std::jthread> threads;
        for (int c = 0; c < kClients; ++c) {
            threads.emplace_back([&, c] {
                Client client;
                if (!client.connect("127.0.0.1", fixture.port(), 10s).ok) {
                    return;
                }
                for (int i = 0; i < kPerClient; ++i) {
                    client.set("c" + std::to_string(c) + ":" + std::to_string(i), "v");
                }
            });
        }
    }

    Server::Config config;
    config.aof_path = dir.aof();
    TestServer fixture(config);

    REQUIRE(fixture.server.store().size() == kClients * kPerClient);
}
