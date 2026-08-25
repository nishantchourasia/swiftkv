/// swiftkv-server -- the key-value server daemon.

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "swiftkv/server.hpp"

namespace {

std::atomic<bool> g_stop{false};

/// Signal handler. Async-signal-safe: it only stores to an atomic flag.
///
/// Doing real work here -- logging, locking, closing sockets -- would be
/// undefined behaviour, because a signal can arrive in the middle of any
/// operation, including one already holding the lock the handler wants.
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

[[noreturn]] void usage(int code) {
    std::cout << R"(swiftkv-server -- distributed key-value store

Usage: swiftkv-server [options]

Options:
  --host <addr>            Bind address                  (default 127.0.0.1)
  --port <n>               Port; 0 picks a free one      (default 6380)
  --io-threads <n>         Event loops; 0 = one per core (default 0)
  --shards <n>             Store shards, rounded to 2^k  (default 64)
  --max-entries <n>        Entries per shard             (default 100000)
  --max-bytes <n>          Bytes per shard; 0 unbounded  (default 0)
  --max-connections <n>    Simultaneous clients          (default 10000)
  --idle-timeout <secs>    Close idle clients; 0 = never (default 300)
  --max-value-bytes <n>    Largest accepted argument     (default 8388608)
  --aof <path>             Append-only log; enables persistence
  --aof-sync <policy>      always | everysec | never      (default everysec)
  --admin-port <n>         Enable HTTP dashboard/metrics on this port
  --admin-host <addr>      Admin bind address             (default 127.0.0.1)
  -h, --help               Show this message

Bind to 127.0.0.1 unless you intend to expose the server: it has no
authentication, so anything that can reach the port can read and write
every key.
)";
    std::exit(code);
}

/// Parse a non-negative integer argument, exiting on anything malformed.
///
/// Rejects rather than clamping: silently reinterpreting "--port 99999" as
/// something else would start a server on a port the operator did not ask for.
unsigned long long parse_number(const char* flag, const char* text) {
    try {
        std::size_t consumed = 0;
        const std::string value(text);
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        std::cerr << "swiftkv-server: invalid value for " << flag << ": " << text << "\n";
        std::exit(2);
    }
}

}  // namespace

int main(int argc, char** argv) {
    swiftkv::Server::Config config;
    config.store.shards = 64;

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto next = [&](void) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "swiftkv-server: " << flag << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (flag == "-h" || flag == "--help") {
            usage(0);
        } else if (flag == "--host") {
            config.host = next();
        } else if (flag == "--port") {
            config.port = static_cast<std::uint16_t>(parse_number("--port", next()));
        } else if (flag == "--io-threads") {
            config.io_threads = parse_number("--io-threads", next());
        } else if (flag == "--shards") {
            config.store.shards = parse_number("--shards", next());
        } else if (flag == "--max-entries") {
            config.store.max_entries_per_shard = parse_number("--max-entries", next());
        } else if (flag == "--max-bytes") {
            config.store.max_bytes_per_shard = parse_number("--max-bytes", next());
        } else if (flag == "--max-connections") {
            config.max_connections = parse_number("--max-connections", next());
        } else if (flag == "--idle-timeout") {
            config.idle_timeout =
                std::chrono::seconds(parse_number("--idle-timeout", next()));
        } else if (flag == "--max-value-bytes") {
            config.limits.max_arg_bytes = parse_number("--max-value-bytes", next());
        } else if (flag == "--admin-port") {
            config.admin_port = static_cast<std::uint16_t>(parse_number("--admin-port", next()));
            config.admin_enabled = true;
        } else if (flag == "--admin-host") {
            config.admin_host = next();
        } else if (flag == "--aof") {
            config.aof_path = next();
        } else if (flag == "--aof-sync") {
            const std::string policy = next();
            if (!swiftkv::parse_sync_policy(policy, config.aof_sync)) {
                std::cerr << "swiftkv-server: --aof-sync must be always, everysec or never\n";
                return 2;
            }
        } else {
            std::cerr << "swiftkv-server: unknown option " << flag << "\n";
            usage(2);
        }
    }

    // SIGPIPE would kill the process when a client disappears mid-write. The
    // socket writes pass MSG_NOSIGNAL, but ignoring it here too covers any
    // other descriptor and costs nothing.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    swiftkv::Server server(config);
    const auto result = server.start();
    if (!result.ok) {
        std::cerr << "swiftkv-server: failed to start: " << result.message << "\n";
        return 1;
    }

    std::cout << "swiftkv-server listening on " << config.host << ":" << server.port() << "\n"
              << "  event loops     : " << (config.io_threads == 0
                                                ? std::thread::hardware_concurrency()
                                                : config.io_threads)
              << "\n"
              << "  store shards    : " << server.store().shard_count() << "\n"
              << "  max connections : " << config.max_connections << "\n"
              << "  idle timeout    : " << config.idle_timeout.count() << "s\n";
    if (config.aof_path.empty()) {
        std::cout << "  persistence     : disabled (in-memory cache only)\n";
    } else {
        const auto& replayed = server.replay_result();
        std::cout << "  persistence     : " << config.aof_path << " (sync="
                  << swiftkv::to_string(config.aof_sync) << ")\n"
                  << "  recovered       : " << replayed.commands_applied
                  << " commands, " << server.store().size() << " keys\n";
        if (replayed.bytes_discarded > 0) {
            std::cout << "  note            : discarded " << replayed.bytes_discarded
                      << " trailing bytes (partial record from an unclean shutdown)\n";
        }
    }
    if (config.admin_enabled) {
        if (server.admin_port() != 0) {
            std::cout << "  dashboard       : http://" << config.admin_host << ":"
                      << server.admin_port() << "/\n"
                      << "  endpoints       : /health /ready /metrics /stats.json\n";
        } else {
            std::cout << "  dashboard       : FAILED to bind (server still serving data)\n";
        }
    }
    std::cout << "ready. press Ctrl-C to stop." << std::endl;

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nshutting down..." << std::endl;
    server.stop();

    const auto& metrics = server.metrics();
    std::cout << "served " << metrics.commands_total.load() << " commands across "
              << metrics.connections_accepted.load() << " connections\n";
    return 0;
}
