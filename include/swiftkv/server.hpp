#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "swiftkv/admin.hpp"
#include "swiftkv/commands.hpp"
#include "swiftkv/net.hpp"
#include "swiftkv/persistence.hpp"
#include "swiftkv/protocol.hpp"
#include "swiftkv/store.hpp"

namespace swiftkv {

/// A TCP server for the key-value store.
///
/// ### Design: several event loops, not a thread per client
///
/// The straightforward design gives every connection its own thread. It is easy
/// to write and it collapses at scale: a thread costs about 8 MB of stack
/// address space, so ten thousand clients is a machine spending all of its time
/// context-switching between mostly-idle threads.
///
/// This server instead runs a small number of **event loops** -- by default one
/// per core -- each owning an `epoll` instance and many connections. A loop
/// sleeps in `epoll_wait` until one of its sockets is actually readable, then
/// services just those. Ten thousand idle connections cost ten thousand
/// descriptors and almost no CPU.
///
/// > **What is epoll?** A kernel facility that lets one thread wait on
/// > thousands of sockets at once and be told exactly which became ready. The
/// > older `select` had to scan every descriptor on every call, so its cost
/// > grew with the number of connections even when nothing happened.
///
/// A connection is owned by exactly one loop for its whole life. That is the
/// property that keeps the design simple: no two threads ever touch the same
/// connection, so connection state needs no locking at all. The only shared
/// state is the `Store`, which is sharded and locked internally.
///
/// ### Level-triggered, deliberately
///
/// `epoll` offers edge-triggered mode, which reports a socket once when it
/// becomes ready. It is marginally faster and notoriously easy to get wrong: if
/// the handler does not drain the socket until `EAGAIN`, the remaining bytes
/// are never announced again and the connection hangs forever. Level-triggered
/// mode re-reports readiness while data remains, so a partial read is merely
/// slower rather than fatal. Correctness first.
class Server {
public:
    struct Config {
        std::string host = "127.0.0.1";

        /// Port 0 asks the kernel for any free port; read the real one back
        /// with `port()` afterwards. Tests rely on this to avoid collisions.
        std::uint16_t port = 6380;

        /// Number of event loops. 0 means "one per hardware thread".
        std::size_t io_threads = 0;

        /// Hard cap on simultaneous connections. Past this, new connections are
        /// accepted and immediately refused with an error, rather than left to
        /// exhaust the process's descriptor limit -- at which point the server
        /// could not accept *any* connection, including an operator's.
        std::size_t max_connections = 10'000;

        /// Close a connection that has sent nothing for this long. Without it,
        /// a client that vanishes without closing (a killed process, a dropped
        /// network) holds its descriptor until the process restarts.
        std::chrono::seconds idle_timeout{300};

        int backlog = 512;

        /// Per-connection read buffer growth cap and per-argument limits.
        Limits limits;

        Store::Config store;

        /// Path to the append-only log. Empty disables persistence entirely,
        /// which is the right setting for a pure cache and for most tests.
        std::string aof_path;

        AppendOnlyLog::SyncPolicy aof_sync = AppendOnlyLog::SyncPolicy::EverySecond;

        /// Serve /health, /ready, /metrics, /stats.json and the dashboard over
        /// HTTP on a second port. Off by default: a process should not open a
        /// port nobody asked for.
        bool admin_enabled = false;
        std::string admin_host = "127.0.0.1";
        std::uint16_t admin_port = 6381;
    };

    explicit Server(Config config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Bind the listening socket and start serving. Returns immediately;
    /// serving continues on background threads.
    NetResult start();

    /// Stop serving and join all threads. Idempotent.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /// The port actually bound. Meaningful only after a successful start().
    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }

    [[nodiscard]] Store& store() noexcept { return store_; }
    [[nodiscard]] ServerMetrics& metrics() noexcept { return metrics_; }
    [[nodiscard]] const ServerMetrics& metrics() const noexcept { return metrics_; }

    /// Snapshot of server state, in the format INFO returns.
    [[nodiscard]] std::string info() const { return executor_.info(); }

    /// Prometheus-style metrics.
    [[nodiscard]] std::string metrics_text() const { return executor_.metrics_text(); }

    /// The append-only log, or nullptr when persistence is disabled.
    [[nodiscard]] AppendOnlyLog* log() noexcept { return log_.get(); }

    /// Outcome of replaying the log at startup.
    [[nodiscard]] const AppendOnlyLog::ReplayResult& replay_result() const noexcept {
        return replay_result_;
    }

    /// JSON snapshot, as served at /stats.json.
    [[nodiscard]] std::string stats_json() const { return executor_.stats_json(); }

    /// The admin HTTP port actually bound, or 0 when the endpoint is disabled.
    [[nodiscard]] std::uint16_t admin_port() const noexcept {
        return admin_ ? admin_->port() : 0;
    }

    /// Whether the server is ready to accept traffic. Reported by /ready.
    ///
    /// Distinct from /health on purpose: a process can be alive but not yet
    /// able to serve -- during log replay, for instance. An orchestrator uses
    /// liveness to decide whether to restart and readiness to decide whether to
    /// send traffic, and conflating them causes a slow-starting server to be
    /// killed instead of waited for.
    [[nodiscard]] bool ready() const noexcept { return running_.load() && !stopping_.load(); }

private:
    /// One client connection. Touched only by its owning loop thread.
    struct Connection {
        FileDescriptor fd;
        std::string inbox;   ///< bytes read, not yet parsed
        std::string outbox;  ///< bytes to write, not yet sent
        std::size_t written = 0;
        bool close_after_write = false;
        std::chrono::steady_clock::time_point last_active;

        explicit Connection(FileDescriptor descriptor)
            : fd(std::move(descriptor)), last_active(std::chrono::steady_clock::now()) {}
    };

    /// One event loop: an epoll instance, its connections, and the thread
    /// running it.
    struct Loop {
        FileDescriptor epoll;
        FileDescriptor wakeup;  ///< eventfd used to interrupt epoll_wait
        std::thread thread;

        /// Connections owned by this loop, keyed by descriptor. Only the loop
        /// thread reads or writes this.
        std::unordered_map<int, std::unique_ptr<Connection>> connections;

        /// Descriptors handed over by the acceptor thread, awaiting adoption.
        /// This is the one place two threads meet, so it is mutex-guarded.
        std::mutex handoff_mutex;
        std::vector<int> handoff;
    };

    void run_acceptor();
    void run_loop(Loop& loop);

    void adopt_pending(Loop& loop);
    void handle_readable(Loop& loop, Connection& connection);
    void handle_writable(Loop& loop, Connection& connection);
    void process_inbox(Connection& connection);
    void flush(Loop& loop, Connection& connection);
    void update_interest(Loop& loop, Connection& connection);
    void close_connection(Loop& loop, int fd);
    void sweep_idle(Loop& loop);
    void wake(Loop& loop);

    /// Build the routing function the admin HTTP server calls.
    HttpAdminServer::Handler make_admin_handler();

    Config config_;
    Store store_;
    ServerMetrics metrics_;
    CommandExecutor executor_;
    std::unique_ptr<AppendOnlyLog> log_;
    AppendOnlyLog::ReplayResult replay_result_;
    std::unique_ptr<HttpAdminServer> admin_;

    /// Listening socket. Touched only by the acceptor thread once serving has
    /// begun, and closed only after that thread has been joined.
    FileDescriptor listener_;

    /// The acceptor's own epoll instance and the eventfd used to interrupt it.
    /// Without the eventfd, shutdown would have to wait out the poll timeout.
    FileDescriptor acceptor_epoll_;
    FileDescriptor acceptor_wakeup_;

    std::uint16_t bound_port_ = 0;

    std::vector<std::unique_ptr<Loop>> loops_;
    std::thread acceptor_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> next_loop_{0};
};

}  // namespace swiftkv
