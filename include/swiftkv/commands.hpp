#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "swiftkv/persistence.hpp"
#include "swiftkv/protocol.hpp"
#include "swiftkv/store.hpp"

namespace swiftkv {

/// Counters describing what the server has done. Read by the metrics endpoint
/// and the dashboard.
///
/// Every field is atomic and updated with `relaxed` ordering. These are
/// statistics, not synchronisation: no code branches on them, so paying for
/// ordering guarantees on the hot path of every request would buy nothing. The
/// only requirement is that increments are not lost, which relaxed atomics
/// already guarantee.
struct ServerMetrics {
    std::atomic<std::uint64_t> commands_total{0};
    std::atomic<std::uint64_t> gets{0};
    std::atomic<std::uint64_t> sets{0};
    std::atomic<std::uint64_t> deletes{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_rejected{0};
    std::atomic<std::uint64_t> connections_current{0};
    std::atomic<std::uint64_t> bytes_read{0};
    std::atomic<std::uint64_t> bytes_written{0};

    void bump(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) noexcept {
        counter.fetch_add(by, std::memory_order_relaxed);
    }
};

/// What the caller should do with the connection after a command.
enum class Disposition {
    KeepOpen,
    /// The client sent QUIT: reply, then close.
    CloseAfterReply,
};

struct CommandResult {
    std::string reply;
    Disposition disposition = Disposition::KeepOpen;
};

/// Executes commands against a store.
///
/// Deliberately knows nothing about sockets. That separation is what allows the
/// entire command surface to be tested without opening a connection, and it
/// keeps the transport free to change -- the same executor serves the TCP
/// server and, later, replication.
class CommandExecutor {
public:
    CommandExecutor(Store& store, ServerMetrics& metrics)
        : store_(store), metrics_(metrics) {}

    /// Attach an append-only log. When set, every command that changes the
    /// keyspace is recorded before the reply is produced, so an acknowledged
    /// write is one that has reached the log.
    ///
    /// Reads are never logged: replaying a GET would change nothing, and
    /// logging them would multiply the log's size by the read ratio -- here,
    /// roughly tenfold.
    void set_log(AppendOnlyLog* log) noexcept { log_ = log; }

    /// Execute one parsed command and produce its reply.
    CommandResult execute(const Command& command);

    /// Human-readable server statistics, in the `key:value` form Redis uses.
    [[nodiscard]] std::string info() const;

    /// Prometheus-style metrics exposition.
    [[nodiscard]] std::string metrics_text() const;

private:
    CommandResult wrong_arity(const std::string& verb);

    Store& store_;
    ServerMetrics& metrics_;
    AppendOnlyLog* log_ = nullptr;
};

}  // namespace swiftkv
