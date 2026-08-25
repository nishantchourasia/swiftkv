#include "swiftkv/commands.hpp"

#include <string>

#include "catch.hpp"

using namespace swiftkv;

namespace {

/// A fixture holding the store, metrics and executor together.
struct Fixture {
    Store store;
    ServerMetrics metrics;
    CommandExecutor executor{store, metrics};

    CommandResult run(std::vector<std::string> args) {
        Command command;
        command.args = std::move(args);
        return executor.execute(command);
    }

    std::string reply(std::vector<std::string> args) { return run(std::move(args)).reply; }
};

}  // namespace

// ---------------------------------------------------------------------------
// Core commands
// ---------------------------------------------------------------------------

TEST_CASE("SET stores a value and GET returns it", "[commands]") {
    Fixture f;

    REQUIRE(f.reply({"SET", "foo", "bar"}) == encode_simple("OK"));
    REQUIRE(f.reply({"GET", "foo"}) == encode_bulk("bar"));
}

TEST_CASE("GET of a missing key returns null, not an empty value", "[commands]") {
    Fixture f;
    f.run({"SET", "empty", ""});

    REQUIRE(f.reply({"GET", "absent"}) == encode_null());
    REQUIRE(f.reply({"GET", "empty"}) == encode_bulk(""));
    REQUIRE(f.reply({"GET", "absent"}) != f.reply({"GET", "empty"}));
}

TEST_CASE("commands are case-insensitive", "[commands]") {
    Fixture f;

    f.run({"set", "k", "v"});
    REQUIRE(f.reply({"GeT", "k"}) == encode_bulk("v"));
}

TEST_CASE("SET overwrites", "[commands]") {
    Fixture f;
    f.run({"SET", "k", "one"});
    f.run({"SET", "k", "two"});

    REQUIRE(f.reply({"GET", "k"}) == encode_bulk("two"));
}

TEST_CASE("DEL reports how many keys it actually removed", "[commands]") {
    Fixture f;
    f.run({"SET", "a", "1"});
    f.run({"SET", "b", "2"});

    REQUIRE(f.reply({"DEL", "a", "b", "never-existed"}) == encode_integer(2));
    REQUIRE(f.reply({"GET", "a"}) == encode_null());
}

TEST_CASE("EXISTS counts present keys", "[commands]") {
    Fixture f;
    f.run({"SET", "a", "1"});

    REQUIRE(f.reply({"EXISTS", "a"}) == encode_integer(1));
    REQUIRE(f.reply({"EXISTS", "a", "b"}) == encode_integer(1));
    REQUIRE(f.reply({"EXISTS", "b"}) == encode_integer(0));
}

TEST_CASE("EXISTS does not affect recency", "[commands]") {
    // Otherwise a monitoring probe could keep dead keys alive indefinitely.
    Store::Config config;
    config.shards = 1;
    config.max_entries_per_shard = 3;
    Store store(config);
    ServerMetrics metrics;
    CommandExecutor executor(store, metrics);

    auto run = [&](std::vector<std::string> args) {
        Command c;
        c.args = std::move(args);
        return executor.execute(c);
    };

    run({"SET", "a", "1"});
    run({"SET", "b", "2"});
    run({"SET", "c", "3"});
    run({"EXISTS", "a"});
    run({"SET", "d", "4"});

    REQUIRE(run({"EXISTS", "a"}).reply == encode_integer(0));
}

TEST_CASE("DBSIZE counts keys", "[commands]") {
    Fixture f;
    REQUIRE(f.reply({"DBSIZE"}) == encode_integer(0));

    f.run({"SET", "a", "1"});
    f.run({"SET", "b", "2"});

    REQUIRE(f.reply({"DBSIZE"}) == encode_integer(2));
}

TEST_CASE("FLUSHALL empties the store", "[commands]") {
    Fixture f;
    f.run({"SET", "a", "1"});

    REQUIRE(f.reply({"FLUSHALL"}) == encode_simple("OK"));
    REQUIRE(f.reply({"DBSIZE"}) == encode_integer(0));
}

TEST_CASE("PING replies PONG, or echoes its argument", "[commands]") {
    Fixture f;

    REQUIRE(f.reply({"PING"}) == encode_simple("PONG"));
    REQUIRE(f.reply({"PING", "hello"}) == encode_bulk("hello"));
}

TEST_CASE("ECHO returns its argument", "[commands]") {
    Fixture f;
    REQUIRE(f.reply({"ECHO", "hi"}) == encode_bulk("hi"));
}

TEST_CASE("QUIT asks the caller to close the connection", "[commands]") {
    Fixture f;
    const auto result = f.run({"QUIT"});

    REQUIRE(result.reply == encode_simple("OK"));
    REQUIRE(result.disposition == Disposition::CloseAfterReply);
}

TEST_CASE("other commands keep the connection open", "[commands]") {
    Fixture f;
    REQUIRE(f.run({"PING"}).disposition == Disposition::KeepOpen);
}

// ---------------------------------------------------------------------------
// Error handling -- must never crash, never leak internals
// ---------------------------------------------------------------------------

TEST_CASE("an unknown command is an error, not a crash", "[commands][errors]") {
    Fixture f;
    const std::string reply = f.reply({"NOTACOMMAND", "x"});

    REQUIRE(reply.rfind("-ERR unknown command", 0) == 0);
    REQUIRE(f.metrics.errors.load() == 1);
}

TEST_CASE("an empty command is an error", "[commands][errors]") {
    Fixture f;
    REQUIRE(f.reply({}).rfind("-ERR", 0) == 0);
}

TEST_CASE("wrong argument counts are rejected", "[commands][errors]") {
    Fixture f;

    for (const auto& args : std::vector<std::vector<std::string>>{
             {"GET"},
             {"GET", "a", "b"},
             {"SET", "a"},
             {"SET", "a", "b", "c"},
             {"DEL"},
             {"EXISTS"},
             {"ECHO"},
             {"DBSIZE", "x"},
             {"FLUSHALL", "x"},
             {"PING", "a", "b"},
         }) {
        INFO("args: " << args.at(0) << " (" << args.size() << ")");
        REQUIRE(f.reply(args).rfind("-ERR wrong number of arguments", 0) == 0);
    }
}

TEST_CASE("an error reply is a single well-formed line", "[commands][errors]") {
    // A key name containing CRLF must not be able to split the reply stream.
    Fixture f;
    const std::string reply = f.reply({"BADCMD\r\n+INJECTED"});

    REQUIRE(reply.find("\r\n") == reply.size() - 2);
}

TEST_CASE("errors do not disturb stored data", "[commands][errors]") {
    Fixture f;
    f.run({"SET", "k", "v"});

    f.run({"NOTACOMMAND"});
    f.run({"GET"});

    REQUIRE(f.reply({"GET", "k"}) == encode_bulk("v"));
}

// ---------------------------------------------------------------------------
// Binary safety
// ---------------------------------------------------------------------------

TEST_CASE("keys and values may contain arbitrary bytes", "[commands]") {
    Fixture f;
    const std::string key = std::string("key\0with\r\nbytes", 15);
    const std::string value = std::string("value\0\r\n\xff", 9);

    f.run({"SET", key, value});

    REQUIRE(f.reply({"GET", key}) == encode_bulk(value));
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

TEST_CASE("counters track command types", "[commands][metrics]") {
    Fixture f;

    f.run({"SET", "a", "1"});
    f.run({"GET", "a"});
    f.run({"GET", "missing"});
    f.run({"DEL", "a"});

    REQUIRE(f.metrics.commands_total.load() == 4);
    REQUIRE(f.metrics.sets.load() == 1);
    REQUIRE(f.metrics.gets.load() == 2);
    REQUIRE(f.metrics.deletes.load() == 1);
}

TEST_CASE("DEL counts only keys that existed", "[commands][metrics]") {
    Fixture f;
    f.run({"SET", "a", "1"});

    f.run({"DEL", "a", "b", "c"});

    REQUIRE(f.metrics.deletes.load() == 1);
}

TEST_CASE("INFO reports keyspace and client state", "[commands][metrics]") {
    Fixture f;
    f.run({"SET", "a", "1"});
    f.run({"GET", "a"});
    f.run({"GET", "missing"});

    const std::string info = f.executor.info();

    REQUIRE(info.find("keys:1") != std::string::npos);
    REQUIRE(info.find("keyspace_hits:1") != std::string::npos);
    REQUIRE(info.find("keyspace_misses:1") != std::string::npos);
    REQUIRE(info.find("total_commands_processed:3") != std::string::npos);
}

TEST_CASE("INFO is returned as a bulk string", "[commands][metrics]") {
    Fixture f;
    REQUIRE(f.reply({"INFO"}).front() == '$');
}

TEST_CASE("metrics exposition is Prometheus-shaped", "[commands][metrics]") {
    Fixture f;
    f.run({"SET", "a", "1"});

    const std::string text = f.executor.metrics_text();

    REQUIRE(text.find("# HELP swiftkv_commands_total") != std::string::npos);
    REQUIRE(text.find("# TYPE swiftkv_commands_total counter") != std::string::npos);
    REQUIRE(text.find("swiftkv_sets_total 1") != std::string::npos);
    REQUIRE(text.find("# TYPE swiftkv_keys gauge") != std::string::npos);
}

TEST_CASE("hit rate reflects lookups", "[commands][metrics]") {
    Fixture f;
    f.run({"SET", "a", "1"});
    f.run({"GET", "a"});
    f.run({"GET", "a"});
    f.run({"GET", "missing"});

    REQUIRE(f.store.stats().hit_rate() == Approx(2.0 / 3.0));
}
