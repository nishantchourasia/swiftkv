#include "swiftkv/commands.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace swiftkv {
namespace {

/// Nanoseconds to microseconds. The cast is explicit because -Wconversion is
/// on: an implicit uint64 -> double narrowing is exactly the kind of silent
/// precision loss that flag exists to surface.
double ns_to_us(std::uint64_t nanos) noexcept {
    return static_cast<double>(nanos) / 1000.0;
}

/// Nanoseconds to seconds, the unit Prometheus expects for durations.
double ns_to_seconds(std::uint64_t nanos) noexcept {
    return static_cast<double>(nanos) / 1e9;
}

}  // namespace

CommandResult CommandExecutor::wrong_arity(const std::string& verb) {
    metrics_.bump(metrics_.errors);
    return {encode_error("ERR wrong number of arguments for '" + verb + "' command"),
            Disposition::KeepOpen};
}

CommandResult CommandExecutor::execute(const Command& command) {
    const auto started = std::chrono::steady_clock::now();
    CommandResult result = dispatch(command);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    metrics_.command_latency.record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));

    return result;
}

CommandResult CommandExecutor::dispatch(const Command& command) {
    metrics_.bump(metrics_.commands_total);

    if (command.empty()) {
        metrics_.bump(metrics_.errors);
        return {encode_error("ERR empty command"), Disposition::KeepOpen};
    }

    const std::string verb = command.verb();
    const auto& args = command.args;

    if (verb == "PING") {
        // PING with an argument echoes it, which is how clients measure
        // round-trip latency without a second command.
        if (args.size() == 1) {
            return {encode_simple("PONG"), Disposition::KeepOpen};
        }
        if (args.size() == 2) {
            return {encode_bulk(args[1]), Disposition::KeepOpen};
        }
        return wrong_arity(verb);
    }

    if (verb == "ECHO") {
        if (args.size() != 2) {
            return wrong_arity(verb);
        }
        return {encode_bulk(args[1]), Disposition::KeepOpen};
    }

    if (verb == "GET") {
        if (args.size() != 2) {
            return wrong_arity(verb);
        }
        metrics_.bump(metrics_.gets);
        if (auto value = store_.get(args[1])) {
            return {encode_bulk(*value), Disposition::KeepOpen};
        }
        // Null, not an empty bulk: a missing key and a key holding "" are
        // different answers and clients rely on telling them apart.
        return {encode_null(), Disposition::KeepOpen};
    }

    if (verb == "SET") {
        if (args.size() != 3) {
            return wrong_arity(verb);
        }
        metrics_.bump(metrics_.sets);
        store_.set(args[1], args[2]);
        // Log before replying, so an "OK" the client received is a write the
        // log has accepted.
        if (log_ != nullptr) {
            log_->append({"SET", args[1], args[2]});
        }
        return {encode_simple("OK"), Disposition::KeepOpen};
    }

    if (verb == "DEL") {
        if (args.size() < 2) {
            return wrong_arity(verb);
        }
        // DEL takes many keys and returns how many actually existed.
        std::int64_t removed = 0;
        std::vector<std::string> logged{"DEL"};
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (store_.del(args[i])) {
                ++removed;
                logged.push_back(args[i]);
            }
        }
        metrics_.bump(metrics_.deletes, static_cast<std::uint64_t>(removed));
        // Only keys that actually existed are logged. Recording deletes of
        // absent keys would grow the log without changing what replay produces.
        if (log_ != nullptr && removed > 0) {
            log_->append(logged);
        }
        return {encode_integer(removed), Disposition::KeepOpen};
    }

    if (verb == "EXISTS") {
        if (args.size() < 2) {
            return wrong_arity(verb);
        }
        std::int64_t found = 0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (store_.contains(args[i])) {
                ++found;
            }
        }
        return {encode_integer(found), Disposition::KeepOpen};
    }

    if (verb == "DBSIZE") {
        if (args.size() != 1) {
            return wrong_arity(verb);
        }
        return {encode_integer(static_cast<std::int64_t>(store_.size())),
                Disposition::KeepOpen};
    }

    if (verb == "FLUSHALL") {
        if (args.size() != 1) {
            return wrong_arity(verb);
        }
        store_.clear();
        if (log_ != nullptr) {
            log_->append({"FLUSHALL"});
        }
        return {encode_simple("OK"), Disposition::KeepOpen};
    }

    if (verb == "INFO") {
        return {encode_bulk(info()), Disposition::KeepOpen};
    }

    if (verb == "QUIT") {
        return {encode_simple("OK"), Disposition::CloseAfterReply};
    }

    metrics_.bump(metrics_.errors);
    // The unknown verb is echoed back so a client can debug its own bug, but
    // nothing about server state is disclosed.
    return {encode_error("ERR unknown command '" + args[0] + "'"), Disposition::KeepOpen};
}

std::string CommandExecutor::info() const {
    const auto stats = store_.stats();
    std::ostringstream out;

    out << "# Server\r\n";
    out << "version:" << "0.1.0" << "\r\n";
    out << "shards:" << store_.shard_count() << "\r\n";

    out << "# Clients\r\n";
    out << "connected_clients:" << metrics_.connections_current.load(std::memory_order_relaxed)
        << "\r\n";
    out << "total_connections_accepted:"
        << metrics_.connections_accepted.load(std::memory_order_relaxed) << "\r\n";
    out << "total_connections_rejected:"
        << metrics_.connections_rejected.load(std::memory_order_relaxed) << "\r\n";

    out << "# Keyspace\r\n";
    out << "keys:" << stats.keys << "\r\n";
    out << "bytes:" << stats.bytes << "\r\n";
    out << "evictions:" << stats.evictions << "\r\n";

    out << "# Stats\r\n";
    out << "total_commands_processed:"
        << metrics_.commands_total.load(std::memory_order_relaxed) << "\r\n";
    out << "keyspace_hits:" << stats.hits << "\r\n";
    out << "keyspace_misses:" << stats.misses << "\r\n";
    out << "hit_rate:" << stats.hit_rate() << "\r\n";
    out << "errors:" << metrics_.errors.load(std::memory_order_relaxed) << "\r\n";

    const auto latency = metrics_.command_latency.snapshot();
    out << "# Latency\r\n";
    out << "latency_samples:" << latency.count << "\r\n";
    out << "latency_mean_us:" << latency.mean_ns / 1000.0 << "\r\n";
    out << "latency_p50_us:" << ns_to_us(latency.p50_ns) << "\r\n";
    out << "latency_p95_us:" << ns_to_us(latency.p95_ns) << "\r\n";
    out << "latency_p99_us:" << ns_to_us(latency.p99_ns) << "\r\n";
    out << "latency_p999_us:" << ns_to_us(latency.p999_ns) << "\r\n";
    out << "latency_max_us:" << ns_to_us(latency.max_ns) << "\r\n";

    return out.str();
}

std::string CommandExecutor::metrics_text() const {
    const auto stats = store_.stats();
    std::ostringstream out;

    auto counter = [&out](const char* name, const char* help, std::uint64_t value) {
        out << "# HELP " << name << ' ' << help << '\n';
        out << "# TYPE " << name << " counter\n";
        out << name << ' ' << value << '\n';
    };
    auto gauge = [&out](const char* name, const char* help, double value) {
        out << "# HELP " << name << ' ' << help << '\n';
        out << "# TYPE " << name << " gauge\n";
        out << name << ' ' << value << '\n';
    };

    const auto load = [](const std::atomic<std::uint64_t>& a) {
        return a.load(std::memory_order_relaxed);
    };

    counter("swiftkv_commands_total", "Commands executed.", load(metrics_.commands_total));
    counter("swiftkv_gets_total", "GET commands executed.", load(metrics_.gets));
    counter("swiftkv_sets_total", "SET commands executed.", load(metrics_.sets));
    counter("swiftkv_deletes_total", "Keys removed by DEL.", load(metrics_.deletes));
    counter("swiftkv_errors_total", "Commands that returned an error.", load(metrics_.errors));
    counter("swiftkv_connections_accepted_total", "Connections accepted.",
            load(metrics_.connections_accepted));
    counter("swiftkv_connections_rejected_total",
            "Connections refused because the limit was reached.",
            load(metrics_.connections_rejected));
    counter("swiftkv_bytes_read_total", "Bytes read from clients.", load(metrics_.bytes_read));
    counter("swiftkv_bytes_written_total", "Bytes written to clients.",
            load(metrics_.bytes_written));
    counter("swiftkv_keyspace_hits_total", "Lookups that found a key.", stats.hits);
    counter("swiftkv_keyspace_misses_total", "Lookups that did not.", stats.misses);
    counter("swiftkv_evictions_total", "Keys evicted to stay within capacity.",
            stats.evictions);

    gauge("swiftkv_connections_current", "Connections open now.",
          static_cast<double>(load(metrics_.connections_current)));
    gauge("swiftkv_keys", "Keys currently stored.", static_cast<double>(stats.keys));
    gauge("swiftkv_bytes", "Bytes currently stored.", static_cast<double>(stats.bytes));
    gauge("swiftkv_hit_rate", "Cache hit rate over the process lifetime.", stats.hit_rate());

    // Server-side service time. Exposed as a summary rather than a Prometheus
    // histogram because the buckets here are log-linear and would not map onto
    // Prometheus's fixed le-buckets without losing the resolution that makes
    // the tail readable.
    const auto latency = metrics_.command_latency.snapshot();
    out << "# HELP swiftkv_command_latency_seconds Server-side command service time.\n";
    out << "# TYPE swiftkv_command_latency_seconds summary\n";
    out << "swiftkv_command_latency_seconds{quantile=\"0.5\"} "
        << ns_to_seconds(latency.p50_ns) << '\n';
    out << "swiftkv_command_latency_seconds{quantile=\"0.9\"} "
        << ns_to_seconds(latency.p90_ns) << '\n';
    out << "swiftkv_command_latency_seconds{quantile=\"0.95\"} "
        << ns_to_seconds(latency.p95_ns) << '\n';
    out << "swiftkv_command_latency_seconds{quantile=\"0.99\"} "
        << ns_to_seconds(latency.p99_ns) << '\n';
    out << "swiftkv_command_latency_seconds{quantile=\"0.999\"} "
        << ns_to_seconds(latency.p999_ns) << '\n';
    out << "swiftkv_command_latency_seconds_count " << latency.count << '\n';

    return out.str();
}

double CommandExecutor::uptime_seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

std::string CommandExecutor::stats_json() const {
    const auto stats = store_.stats();
    const auto latency = metrics_.command_latency.snapshot();
    const auto load = [](const std::atomic<std::uint64_t>& a) {
        return a.load(std::memory_order_relaxed);
    };

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << "{\n";
    out << "  \"status\": \"ok\",\n";
    out << "  \"version\": \"0.1.0\",\n";
    out << std::setprecision(3);
    out << "  \"uptime_seconds\": " << uptime_seconds() << ",\n";

    out << "  \"server\": {\n";
    out << "    \"shards\": " << store_.shard_count() << ",\n";
    out << "    \"persistence\": " << (log_ != nullptr ? "true" : "false") << "\n";
    out << "  },\n";

    out << "  \"connections\": {\n";
    out << "    \"current\": " << load(metrics_.connections_current) << ",\n";
    out << "    \"accepted\": " << load(metrics_.connections_accepted) << ",\n";
    out << "    \"rejected\": " << load(metrics_.connections_rejected) << "\n";
    out << "  },\n";

    out << "  \"commands\": {\n";
    out << "    \"total\": " << load(metrics_.commands_total) << ",\n";
    out << "    \"gets\": " << load(metrics_.gets) << ",\n";
    out << "    \"sets\": " << load(metrics_.sets) << ",\n";
    out << "    \"deletes\": " << load(metrics_.deletes) << ",\n";
    out << "    \"errors\": " << load(metrics_.errors) << "\n";
    out << "  },\n";

    out << "  \"keyspace\": {\n";
    out << "    \"keys\": " << stats.keys << ",\n";
    out << "    \"bytes\": " << stats.bytes << ",\n";
    out << "    \"hits\": " << stats.hits << ",\n";
    out << "    \"misses\": " << stats.misses << ",\n";
    out << std::setprecision(4);
    out << "    \"hit_rate\": " << stats.hit_rate() << ",\n";
    out << "    \"evictions\": " << stats.evictions << "\n";
    out << "  },\n";

    out << std::setprecision(3);
    out << "  \"latency_us\": {\n";
    out << "    \"count\": " << latency.count << ",\n";
    out << "    \"mean\": " << latency.mean_ns / 1000.0 << ",\n";
    out << "    \"p50\": " << ns_to_us(latency.p50_ns) << ",\n";
    out << "    \"p90\": " << ns_to_us(latency.p90_ns) << ",\n";
    out << "    \"p95\": " << ns_to_us(latency.p95_ns) << ",\n";
    out << "    \"p99\": " << ns_to_us(latency.p99_ns) << ",\n";
    out << "    \"p999\": " << ns_to_us(latency.p999_ns) << ",\n";
    out << "    \"min\": " << ns_to_us(latency.min_ns) << ",\n";
    out << "    \"max\": " << ns_to_us(latency.max_ns) << "\n";
    out << "  },\n";

    out << "  \"traffic\": {\n";
    out << "    \"bytes_read\": " << load(metrics_.bytes_read) << ",\n";
    out << "    \"bytes_written\": " << load(metrics_.bytes_written) << "\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

}  // namespace swiftkv
