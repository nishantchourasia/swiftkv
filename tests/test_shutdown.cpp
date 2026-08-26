/// Graceful shutdown, mass disconnection, and descriptor-leak tests.
///
/// These cover the failure modes that only appear when a server is stopped or
/// abused rather than merely used: replies dropped on the floor at shutdown,
/// descriptors leaked per connection, and the interaction between a shutdown
/// and clients vanishing at the same moment.

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "swiftkv/client.hpp"
#include "swiftkv/persistence.hpp"
#include "swiftkv/protocol.hpp"
#include "swiftkv/server.hpp"

using namespace swiftkv;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/// Number of file descriptors this process currently holds.
///
/// Read from /proc/self/fd, which is the only way to observe a leak directly.
/// A leaked descriptor is worse than a memory leak: the process hits
/// RLIMIT_NOFILE and can then accept no connection at all.
std::size_t open_fd_count() {
    std::size_t count = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator("/proc/self/fd", ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        ++count;
    }
    return count;
}

Server::Config test_config(std::size_t io_threads = 2) {
    Server::Config config;
    config.host = "127.0.0.1";
    config.port = 0;  // kernel picks a free port
    config.io_threads = io_threads;
    return config;
}

/// A raw TCP connection that can be dropped abruptly.
class RawConnection {
public:
    explicit RawConnection(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    RawConnection(const RawConnection&) = delete;
    RawConnection& operator=(const RawConnection&) = delete;

    ~RawConnection() { close_politely(); }

    [[nodiscard]] bool connected() const { return fd_ >= 0; }

    void send(const std::string& bytes) const {
        if (fd_ < 0) {
            return;
        }
        ssize_t ignored = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        (void)ignored;
    }

    /// Drop the connection with a TCP reset rather than an orderly close.
    ///
    /// SO_LINGER with a zero timeout makes close() send RST instead of FIN.
    /// That is what a killed process or a yanked cable looks like to the
    /// server: no goodbye, and any data still queued is discarded. A polite
    /// close would exercise a much gentler path.
    void abort_now() {
        if (fd_ < 0) {
            return;
        }
        linger reset{};
        reset.l_onoff = 1;
        reset.l_linger = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        ::close(fd_);
        fd_ = -1;
    }

    void close_politely() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

/// A temporary directory for an append-only log.
struct TempDir {
    std::string path;

    TempDir() {
        static std::atomic<int> counter{0};
        path = "/tmp/swiftkv-shutdown-" + std::to_string(::getpid()) + "-" +
               std::to_string(counter.fetch_add(1));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    [[nodiscard]] std::string aof() const { return path + "/appendonly.aof"; }
};

/// Wait until a predicate holds, or a deadline passes. Returns whether it held.
template <typename Fn>
bool wait_until(Fn&& predicate, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

}  // namespace

// ---------------------------------------------------------------------------
// Graceful shutdown
// ---------------------------------------------------------------------------

TEST_CASE("in-flight requests are answered before shutdown closes the connection",
          "[shutdown][graceful]") {
    // The defining property of a graceful stop. The client pipelines commands
    // and does not read; the server is then told to stop. Every reply must
    // still arrive. Without a drain phase the socket would simply be closed and
    // the client would be left with no answer to work it had already sent.
    constexpr int kCommands = 300;

    Server server(test_config());
    REQUIRE(server.start().ok);

    Client client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);

    std::string batch;
    for (int i = 0; i < kCommands; ++i) {
        batch += encode_array({"SET", "k" + std::to_string(i), "v" + std::to_string(i)});
    }
    REQUIRE(client.send_raw(batch).ok);

    // Stop while those commands are still being processed.
    server.stop();

    int answered = 0;
    for (int i = 0; i < kCommands; ++i) {
        auto reply = client.read_reply();
        if (!reply) {
            break;
        }
        REQUIRE(reply->text == "OK");
        ++answered;
    }

    REQUIRE(answered == kCommands);
    REQUIRE(server.drain_completed());
    REQUIRE(server.forced_closed() == 0);
}

TEST_CASE("a large pending reply is fully written before closing",
          "[shutdown][graceful]") {
    // A reply too big for the kernel send buffer needs several EPOLLOUT rounds.
    // The drain must keep the connection open until the last byte is out.
    Server server(test_config());
    REQUIRE(server.start().ok);

    const std::string value(4 * 1024 * 1024, 'z');
    {
        Client writer;
        REQUIRE(writer.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(writer.set("big", value));
    }

    Client client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 10s).ok);
    REQUIRE(client.send_raw(encode_array({"GET", "big"})).ok);

    // The read happens on its own thread, because a real client reads while the
    // server is draining. Reading only after stop() returns would deadlock the
    // test rather than test the server: stop() blocks for the grace period, so
    // nobody would be draining the socket, the kernel send buffer would fill,
    // and the write could never complete.
    std::optional<Reply> reply;
    std::jthread reader([&] { reply = client.read_reply(); });

    server.stop();
    reader.join();

    REQUIRE(reply);
    REQUIRE(reply->text.size() == value.size());
    REQUIRE(reply->text == value);
    REQUIRE(server.drain_completed());
}

TEST_CASE("shutdown stops accepting new connections immediately",
          "[shutdown][graceful]") {
    Server server(test_config());
    REQUIRE(server.start().ok);
    const std::uint16_t port = server.port();

    Client existing;
    REQUIRE(existing.connect("127.0.0.1", port, 5s).ok);
    REQUIRE(existing.ping());

    server.stop();

    // The listening socket is closed, so a new connection is refused.
    Client latecomer;
    REQUIRE_FALSE(latecomer.connect("127.0.0.1", port, 1s).ok);
}

TEST_CASE("readiness reports false during shutdown", "[shutdown][graceful]") {
    // A load balancer must stop sending new work while existing work drains.
    Server server(test_config());
    REQUIRE(server.start().ok);
    REQUIRE(server.ready());

    server.stop();

    REQUIRE_FALSE(server.ready());
    REQUIRE_FALSE(server.running());
}

TEST_CASE("stop is idempotent and safe to call repeatedly", "[shutdown][graceful]") {
    Server server(test_config());
    REQUIRE(server.start().ok);

    server.stop();
    server.stop();
    server.stop();

    REQUIRE_FALSE(server.running());
}

TEST_CASE("a server that was never started stops cleanly", "[shutdown][graceful]") {
    Server server(test_config());
    server.stop();
    REQUIRE_FALSE(server.running());
}

TEST_CASE("start and stop can be cycled", "[shutdown][graceful]") {
    Server server(test_config());

    for (int cycle = 0; cycle < 5; ++cycle) {
        INFO("cycle " << cycle);
        REQUIRE(server.start().ok);
        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(client.ping());
        server.stop();
    }
}

TEST_CASE("a client that stops reading cannot hold shutdown open forever",
          "[shutdown][graceful]") {
    // The grace period is bounded on purpose. A client that requests a large
    // value and then never reads it would otherwise block shutdown until the
    // process is killed.
    Server server(test_config());
    REQUIRE(server.start().ok);

    {
        Client writer;
        REQUIRE(writer.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(writer.set("big", std::string(8 * 1024 * 1024, 'x')));
    }

    // Request a large value using a raw socket, then never read the reply.
    RawConnection sulker(server.port());
    REQUIRE(sulker.connected());
    sulker.send(encode_array({"GET", "big"}));
    std::this_thread::sleep_for(100ms);

    const auto started = std::chrono::steady_clock::now();
    server.stop(300ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Bounded: it must not wait indefinitely for a client that will never read.
    REQUIRE(elapsed < 3s);
    REQUIRE_FALSE(server.running());
}

// ---------------------------------------------------------------------------
// Persistence across a graceful shutdown
// ---------------------------------------------------------------------------

TEST_CASE("the log is flushed by a graceful shutdown", "[shutdown][persistence]") {
    // With sync=never nothing is fsynced during normal operation, so if the
    // data survives it can only be because shutdown flushed it. That makes this
    // a direct test of the flush rather than of the sync policy.
    TempDir dir;

    {
        Server::Config config = test_config();
        config.aof_path = dir.aof();
        config.aof_sync = AppendOnlyLog::SyncPolicy::Never;
        Server server(config);
        REQUIRE(server.start().ok);

        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);
        for (int i = 0; i < 200; ++i) {
            REQUIRE(client.set("k" + std::to_string(i), "v" + std::to_string(i)));
        }

        server.stop();
    }

    Server::Config config = test_config();
    config.aof_path = dir.aof();
    Server restarted(config);
    REQUIRE(restarted.start().ok);

    REQUIRE(restarted.store().size() == 200);
    REQUIRE(restarted.store().get("k42") == "v42");
    restarted.stop();
}

TEST_CASE("in-flight writes are durable across a graceful shutdown",
          "[shutdown][persistence]") {
    // Commands still being processed when the stop arrives must be both
    // answered and persisted -- an acknowledged write that vanished on restart
    // would be a broken promise.
    TempDir dir;
    constexpr int kCommands = 200;

    {
        Server::Config config = test_config();
        config.aof_path = dir.aof();
        config.aof_sync = AppendOnlyLog::SyncPolicy::EverySecond;
        Server server(config);
        REQUIRE(server.start().ok);

        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);

        std::string batch;
        for (int i = 0; i < kCommands; ++i) {
            batch += encode_array({"SET", "inflight" + std::to_string(i), "v"});
        }
        REQUIRE(client.send_raw(batch).ok);

        server.stop();

        // Everything acknowledged must also be durable.
        for (int i = 0; i < kCommands; ++i) {
            auto reply = client.read_reply();
            REQUIRE(reply);
            REQUIRE(reply->text == "OK");
        }
    }

    Server::Config config = test_config();
    config.aof_path = dir.aof();
    Server restarted(config);
    REQUIRE(restarted.start().ok);

    REQUIRE(restarted.store().size() == kCommands);
    restarted.stop();
}

TEST_CASE("repeated graceful restarts accumulate data correctly",
          "[shutdown][persistence]") {
    TempDir dir;

    for (int round = 0; round < 4; ++round) {
        Server::Config config = test_config();
        config.aof_path = dir.aof();
        Server server(config);
        REQUIRE(server.start().ok);

        INFO("round " << round);
        REQUIRE(server.store().size() == static_cast<std::size_t>(round) * 10);

        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);
        for (int i = 0; i < 10; ++i) {
            REQUIRE(client.set("r" + std::to_string(round) + ":" + std::to_string(i), "v"));
        }
        server.stop();
    }

    Server::Config config = test_config();
    config.aof_path = dir.aof();
    Server final_server(config);
    REQUIRE(final_server.start().ok);
    REQUIRE(final_server.store().size() == 40);
    final_server.stop();
}

// ---------------------------------------------------------------------------
// Mass disconnection
// ---------------------------------------------------------------------------

TEST_CASE("many clients disconnecting abruptly mid-request", "[shutdown][disconnect]") {
    // Every client sends a command and then resets the connection without
    // reading the reply. The server discovers the peer is gone only when it
    // tries to write, so this exercises the EPIPE/ECONNRESET path on the write
    // side rather than the tidy EOF path on the read side.
    constexpr int kClients = 200;

    Server server(test_config(4));
    REQUIRE(server.start().ok);

    {
        Client seeder;
        REQUIRE(seeder.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(seeder.set("payload", std::string(256 * 1024, 'p')));
    }

    for (int i = 0; i < kClients; ++i) {
        RawConnection connection(server.port());
        if (!connection.connected()) {
            continue;
        }
        connection.send(encode_array({"GET", "payload"}));
        connection.abort_now();  // RST, mid-reply
    }

    // The server must still be healthy.
    Client survivor;
    REQUIRE(survivor.connect("127.0.0.1", server.port(), 5s).ok);
    REQUIRE(survivor.ping());
    REQUIRE(survivor.set("after", "mass-disconnect"));
    REQUIRE(survivor.get("after") == "mass-disconnect");

    server.stop();
}

TEST_CASE("clients disconnecting from many threads at once", "[shutdown][disconnect]") {
    constexpr int kThreads = 16;
    constexpr int kPerThread = 25;

    Server server(test_config(4));
    REQUIRE(server.start().ok);

    std::atomic<int> completed{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) {
                    RawConnection connection(server.port());
                    if (!connection.connected()) {
                        continue;
                    }
                    connection.send(encode_array({"SET", "k", "v"}));
                    // Half reset, half close politely: both paths matter.
                    if (i % 2 == 0) {
                        connection.abort_now();
                    } else {
                        connection.close_politely();
                    }
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }

    REQUIRE(completed.load() > 0);

    Client survivor;
    REQUIRE(survivor.connect("127.0.0.1", server.port(), 5s).ok);
    REQUIRE(survivor.ping());

    server.stop();
}

TEST_CASE("connections dropped part-way through a command", "[shutdown][disconnect]") {
    // A truncated command leaves bytes in the server's parse buffer that will
    // never complete. Dropping the connection must release that buffer rather
    // than leave it pinned.
    constexpr int kClients = 150;

    Server server(test_config(4));
    REQUIRE(server.start().ok);

    for (int i = 0; i < kClients; ++i) {
        RawConnection connection(server.port());
        if (!connection.connected()) {
            continue;
        }
        // Announce three arguments, then send only part of the first.
        connection.send("*3\r\n$3\r\nSET\r\n$10\r\npart");
        connection.abort_now();
    }

    Client survivor;
    REQUIRE(survivor.connect("127.0.0.1", server.port(), 5s).ok);
    REQUIRE(survivor.ping());

    server.stop();
}

TEST_CASE("mass disconnection concurrent with shutdown", "[shutdown][disconnect]") {
    // The interesting interleaving: clients vanishing at the exact moment the
    // server is tearing itself down. Neither path may deadlock or crash.
    constexpr int kThreads = 8;

    Server server(test_config(4));
    REQUIRE(server.start().ok);
    const std::uint16_t port = server.port();

    std::atomic<bool> go{false};
    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 40; ++i) {
                RawConnection connection(port);
                if (!connection.connected()) {
                    continue;  // listener already closed; expected
                }
                connection.send(encode_array({"SET", "racing", "value"}));
                connection.abort_now();
            }
        });
    }

    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(20ms);  // let some connections land
    server.stop();
    threads.clear();  // join

    REQUIRE_FALSE(server.running());
}

TEST_CASE("the connection gauge returns to zero after mass disconnection",
          "[shutdown][disconnect]") {
    // A gauge that drifts upward means connections are being counted in but not
    // out, which is the metric-level symptom of a descriptor leak.
    constexpr int kClients = 100;

    Server server(test_config(2));
    REQUIRE(server.start().ok);

    for (int i = 0; i < kClients; ++i) {
        RawConnection connection(server.port());
        if (connection.connected()) {
            connection.send(encode_array({"PING"}));
            connection.abort_now();
        }
    }

    const bool settled = wait_until(
        [&] { return server.metrics().connections_current.load() == 0; }, 10s);

    INFO("connections_current = " << server.metrics().connections_current.load());
    REQUIRE(settled);

    server.stop();
}

// ---------------------------------------------------------------------------
// Descriptor leaks
// ---------------------------------------------------------------------------

TEST_CASE("start and stop cycles leak no descriptors", "[shutdown][fd]") {
    // Each cycle opens a listener, an epoll, two eventfds and several loop
    // epolls. If any is not closed, the count climbs with every cycle.
    (void)open_fd_count();  // warm any lazily-opened descriptor

    const std::size_t before = open_fd_count();

    for (int cycle = 0; cycle < 10; ++cycle) {
        Server server(test_config(4));
        REQUIRE(server.start().ok);
        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(client.ping());
        server.stop();
    }

    const std::size_t after = open_fd_count();
    INFO("fd count before " << before << ", after " << after);
    REQUIRE(after <= before);
}

TEST_CASE("connection churn leaks no descriptors", "[shutdown][fd]") {
    Server server(test_config(2));
    REQUIRE(server.start().ok);

    // Settle first: the first connections allocate structures that persist.
    for (int i = 0; i < 20; ++i) {
        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(client.ping());
    }
    REQUIRE(wait_until([&] { return server.metrics().connections_current.load() == 0; }));

    const std::size_t before = open_fd_count();

    for (int i = 0; i < 200; ++i) {
        Client client;
        REQUIRE(client.connect("127.0.0.1", server.port(), 5s).ok);
        REQUIRE(client.ping());
    }
    REQUIRE(wait_until([&] { return server.metrics().connections_current.load() == 0; }));

    const std::size_t after = open_fd_count();
    INFO("fd count before " << before << ", after " << after);
    REQUIRE(after <= before + 2);  // slack for anything the runtime opens lazily

    server.stop();
}

TEST_CASE("abruptly reset connections leak no descriptors", "[shutdown][fd]") {
    Server server(test_config(2));
    REQUIRE(server.start().ok);

    for (int i = 0; i < 20; ++i) {
        RawConnection connection(server.port());
        connection.send(encode_array({"PING"}));
        connection.abort_now();
    }
    REQUIRE(wait_until([&] { return server.metrics().connections_current.load() == 0; }));

    const std::size_t before = open_fd_count();

    for (int i = 0; i < 300; ++i) {
        RawConnection connection(server.port());
        if (connection.connected()) {
            connection.send(encode_array({"PING"}));
            connection.abort_now();
        }
    }
    REQUIRE(wait_until([&] { return server.metrics().connections_current.load() == 0; }, 15s));

    const std::size_t after = open_fd_count();
    INFO("fd count before " << before << ", after " << after);
    REQUIRE(after <= before + 2);

    server.stop();
}

TEST_CASE("a shutdown racing the acceptor leaks no descriptors", "[shutdown][fd]") {
    // Connections can be accepted and queued for handoff at the instant the
    // acceptor stops. Those descriptors were never registered with epoll, so
    // shutdown has to close them explicitly or they leak.
    const std::size_t before = open_fd_count();

    for (int cycle = 0; cycle < 5; ++cycle) {
        Server server(test_config(2));
        REQUIRE(server.start().ok);
        const std::uint16_t port = server.port();

        std::atomic<bool> stop_connecting{false};
        std::jthread connector([&] {
            while (!stop_connecting.load(std::memory_order_acquire)) {
                RawConnection connection(port);
                if (connection.connected()) {
                    connection.send(encode_array({"PING"}));
                }
            }
        });

        std::this_thread::sleep_for(30ms);
        server.stop();
        stop_connecting.store(true, std::memory_order_release);
    }

    const std::size_t after = open_fd_count();
    INFO("fd count before " << before << ", after " << after);
    REQUIRE(after <= before + 2);
}
