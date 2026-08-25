/// swiftkv-bench -- load generator and latency profiler.
///
/// Purpose-built rather than borrowed. k6, wrk and ApacheBench all speak HTTP;
/// SwiftKV speaks RESP over a raw TCP socket, so no off-the-shelf tool can
/// drive it. Redis ships `redis-benchmark` for exactly this reason.
///
/// ### What is measured
///
/// Each worker owns one connection and issues one command at a time, waiting
/// for the reply before sending the next. The recorded latency is therefore the
/// full client-observed round trip: serialise, write, server work, read, parse.
///
/// ### What is NOT measured, stated plainly
///
/// This is a **closed-loop** benchmark. A worker that is waiting for a slow
/// reply is not issuing new requests, so the offered load falls when the server
/// slows down. That means these numbers describe *service time at the
/// throughput actually achieved*, not the latency a fixed arrival rate would
/// see. The effect is known as coordinated omission, and it makes closed-loop
/// percentiles optimistic under saturation. Concurrency is varied across runs
/// instead, so the shape of the curve still shows where the server degrades.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "swiftkv/client.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    std::size_t connections = 50;
    std::size_t requests = 10'000;   ///< per connection, after warmup
    std::size_t warmup = 1'000;      ///< per connection, discarded
    std::size_t value_size = 64;
    std::size_t keyspace = 100'000;
    int read_percent = 90;
    bool csv = false;
    std::string label;
};

struct WorkerResult {
    std::vector<std::uint64_t> latencies_ns;
    std::uint64_t errors = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

[[noreturn]] void usage(int code) {
    std::cout << R"(swiftkv-bench -- load generator and latency profiler

Usage: swiftkv-bench [options]

Options:
  --host <addr>        Server address                 (default 127.0.0.1)
  --port <n>           Server port                    (default 6380)
  --connections <n>    Concurrent connections         (default 50)
  --requests <n>       Measured requests per conn     (default 10000)
  --warmup <n>         Discarded requests per conn    (default 1000)
  --value-size <n>     Bytes per value                (default 64)
  --keyspace <n>       Distinct keys                  (default 100000)
  --read-percent <n>   Share of GETs, 0-100           (default 90)
  --csv                Emit one CSV row instead of a table
  --label <text>       Label for the CSV row
  -h, --help           Show this message
)";
    std::exit(code);
}

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
        std::cerr << "swiftkv-bench: invalid value for " << flag << ": " << text << "\n";
        std::exit(2);
    }
}

/// Nearest-rank percentile over sorted data.
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    const double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(std::llround(rank));
    return sorted[std::min(index, sorted.size() - 1)];
}

double to_ms(std::uint64_t nanos) { return static_cast<double>(nanos) / 1e6; }

void run_worker(const Options& options, std::size_t worker_id, WorkerResult& result,
                std::atomic<std::size_t>& failed_connections) {
    swiftkv::Client client;
    if (!client.connect(options.host, options.port, std::chrono::seconds(10)).ok) {
        failed_connections.fetch_add(1);
        return;
    }

    // Each worker gets its own generator, seeded distinctly. Sharing one would
    // serialise the workers on its internal state and measure the benchmark
    // rather than the server.
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ worker_id);
    std::uniform_int_distribution<std::size_t> key_dist(0, options.keyspace - 1);
    std::uniform_int_distribution<int> op_dist(1, 100);

    const std::string value(options.value_size, 'v');

    // Keys are built once and reused. Formatting a string inside the timed loop
    // would put the allocator on the hot path and inflate the latency figures.
    std::vector<std::string> keys;
    keys.reserve(options.keyspace);
    for (std::size_t i = 0; i < options.keyspace; ++i) {
        keys.push_back("bench:key:" + std::to_string(i));
    }

    const std::size_t total = options.warmup + options.requests;
    result.latencies_ns.reserve(options.requests);

    for (std::size_t i = 0; i < total; ++i) {
        const std::string& key = keys[key_dist(rng)];
        const bool is_read = op_dist(rng) <= options.read_percent;

        const auto started = Clock::now();
        std::optional<swiftkv::Reply> reply =
            is_read ? client.command({"GET", key}) : client.command({"SET", key, value});
        const auto elapsed = Clock::now() - started;

        const bool measured = i >= options.warmup;

        if (!reply || reply->is_error()) {
            if (measured) {
                ++result.errors;
            }
            if (!reply) {
                break;  // connection lost; stop this worker
            }
            continue;
        }

        if (measured) {
            result.latencies_ns.push_back(
                static_cast<std::uint64_t>(std::chrono::duration_cast<Nanos>(elapsed).count()));
            if (is_read) {
                reply->is_null() ? ++result.misses : ++result.hits;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "swiftkv-bench: " << flag << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (flag == "-h" || flag == "--help") {
            usage(0);
        } else if (flag == "--host") {
            options.host = next();
        } else if (flag == "--port") {
            options.port = static_cast<std::uint16_t>(parse_number("--port", next()));
        } else if (flag == "--connections") {
            options.connections = parse_number("--connections", next());
        } else if (flag == "--requests") {
            options.requests = parse_number("--requests", next());
        } else if (flag == "--warmup") {
            options.warmup = parse_number("--warmup", next());
        } else if (flag == "--value-size") {
            options.value_size = parse_number("--value-size", next());
        } else if (flag == "--keyspace") {
            options.keyspace = parse_number("--keyspace", next());
        } else if (flag == "--read-percent") {
            options.read_percent = static_cast<int>(parse_number("--read-percent", next()));
        } else if (flag == "--csv") {
            options.csv = true;
        } else if (flag == "--label") {
            options.label = next();
        } else {
            std::cerr << "swiftkv-bench: unknown option " << flag << "\n";
            usage(2);
        }
    }

    if (options.connections == 0 || options.keyspace == 0) {
        std::cerr << "swiftkv-bench: --connections and --keyspace must be non-zero\n";
        return 2;
    }
    if (options.read_percent < 0 || options.read_percent > 100) {
        std::cerr << "swiftkv-bench: --read-percent must be between 0 and 100\n";
        return 2;
    }

    std::vector<WorkerResult> results(options.connections);
    std::atomic<std::size_t> failed_connections{0};

    const auto started = Clock::now();
    {
        std::vector<std::jthread> workers;
        workers.reserve(options.connections);
        for (std::size_t i = 0; i < options.connections; ++i) {
            workers.emplace_back([&, i] {
                run_worker(options, i, results[i], failed_connections);
            });
        }
    }
    const auto wall = Clock::now() - started;

    if (failed_connections.load() == options.connections) {
        std::cerr << "swiftkv-bench: could not connect to " << options.host << ":"
                  << options.port << "\n";
        return 1;
    }

    std::vector<std::uint64_t> latencies;
    std::uint64_t errors = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    for (const auto& result : results) {
        latencies.insert(latencies.end(), result.latencies_ns.begin(),
                         result.latencies_ns.end());
        errors += result.errors;
        hits += result.hits;
        misses += result.misses;
    }

    if (latencies.empty()) {
        std::cerr << "swiftkv-bench: no successful requests\n";
        return 1;
    }

    std::sort(latencies.begin(), latencies.end());

    const double seconds = std::chrono::duration<double>(wall).count();
    const double throughput = static_cast<double>(latencies.size()) / seconds;
    const double mean =
        static_cast<double>(std::accumulate(latencies.begin(), latencies.end(),
                                            static_cast<long double>(0))) /
        static_cast<double>(latencies.size());
    const double error_rate =
        static_cast<double>(errors) / static_cast<double>(latencies.size() + errors) * 100.0;

    if (options.csv) {
        std::cout << std::fixed << std::setprecision(4);
        std::cout << options.label << "," << options.connections << "," << latencies.size()
                  << "," << options.value_size << "," << options.read_percent << ","
                  << std::setprecision(1) << throughput << "," << std::setprecision(4)
                  << to_ms(static_cast<std::uint64_t>(mean)) << ","
                  << to_ms(percentile(latencies, 50)) << ","
                  << to_ms(percentile(latencies, 95)) << ","
                  << to_ms(percentile(latencies, 99)) << ","
                  << to_ms(percentile(latencies, 99.9)) << ","
                  << to_ms(latencies.back()) << "," << errors << ","
                  << std::setprecision(3) << error_rate << "\n";
        return 0;
    }

    std::cout << std::fixed;
    std::cout << "swiftkv-bench\n"
              << "  target        : " << options.host << ":" << options.port << "\n"
              << "  connections   : " << options.connections << "\n"
              << "  requests      : " << latencies.size() << " measured ("
              << options.warmup * options.connections << " warmup discarded)\n"
              << "  value size    : " << options.value_size << " bytes\n"
              << "  read/write    : " << options.read_percent << "% GET / "
              << (100 - options.read_percent) << "% SET\n"
              << "  keyspace      : " << options.keyspace << "\n"
              << "  duration      : " << std::setprecision(2) << seconds << " s\n";
    if (failed_connections.load() > 0) {
        std::cout << "  WARNING       : " << failed_connections.load()
                  << " connections failed\n";
    }
    std::cout << "\n"
              << "  throughput    : " << std::setprecision(0) << throughput << " ops/sec\n"
              << "  error rate    : " << std::setprecision(3) << error_rate << " % ("
              << errors << " errors)\n";
    if (hits + misses > 0) {
        std::cout << "  read hit rate : " << std::setprecision(1)
                  << (static_cast<double>(hits) / static_cast<double>(hits + misses) * 100.0)
                  << " %\n";
    }
    std::cout << "\n"
              << "  latency (ms)\n"
              << std::setprecision(4)
              << "    min         : " << to_ms(latencies.front()) << "\n"
              << "    mean        : " << to_ms(static_cast<std::uint64_t>(mean)) << "\n"
              << "    p50         : " << to_ms(percentile(latencies, 50)) << "\n"
              << "    p95         : " << to_ms(percentile(latencies, 95)) << "\n"
              << "    p99         : " << to_ms(percentile(latencies, 99)) << "\n"
              << "    p99.9       : " << to_ms(percentile(latencies, 99.9)) << "\n"
              << "    max         : " << to_ms(latencies.back()) << "\n";

    return 0;
}
