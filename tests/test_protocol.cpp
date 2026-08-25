#include "swiftkv/protocol.hpp"

#include <string>

#include "catch.hpp"

using namespace swiftkv;

namespace {

/// Encode a command the way a client would, for round-trip tests.
std::string request(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Well-formed input
// ---------------------------------------------------------------------------

TEST_CASE("parses a SET command", "[protocol]") {
    Command command;
    const std::string input = request({"SET", "foo", "bar"});

    const auto result = parse_command(input, command);

    REQUIRE(result.ok());
    REQUIRE(result.consumed == input.size());
    REQUIRE(command.args == std::vector<std::string>{"SET", "foo", "bar"});
}

TEST_CASE("verb is uppercased so commands are case-insensitive", "[protocol]") {
    Command command;
    parse_command(request({"set", "k", "v"}), command);

    REQUIRE(command.verb() == "SET");
}

TEST_CASE("verb of an empty command is empty", "[protocol]") {
    Command command;
    REQUIRE(command.verb().empty());
}

TEST_CASE("parses a command with no arguments", "[protocol]") {
    Command command;
    const auto result = parse_command("*0\r\n", command);

    REQUIRE(result.ok());
    REQUIRE(command.empty());
}

TEST_CASE("parses an empty argument", "[protocol]") {
    // An empty value is legitimate and must stay distinct from a missing one.
    Command command;
    const auto result = parse_command(request({"SET", "k", ""}), command);

    REQUIRE(result.ok());
    REQUIRE(command.args[2].empty());
}

TEST_CASE("values may contain CRLF and NUL bytes", "[protocol]") {
    // This is why the protocol is length-prefixed rather than delimited.
    const std::string nasty = std::string("line1\r\nline2\0binary", 18);
    Command command;

    const auto result = parse_command(request({"SET", "k", nasty}), command);

    REQUIRE(result.ok());
    REQUIRE(command.args[2] == nasty);
}

TEST_CASE("consumes only the first command when several are buffered", "[protocol]") {
    // A busy client pipelines: one read can deliver many commands.
    const std::string first = request({"GET", "a"});
    const std::string second = request({"GET", "b"});
    Command command;

    const auto result = parse_command(first + second, command);

    REQUIRE(result.ok());
    REQUIRE(result.consumed == first.size());
    REQUIRE(command.args == std::vector<std::string>{"GET", "a"});
}

// ---------------------------------------------------------------------------
// Incremental parsing -- TCP does not preserve message boundaries
// ---------------------------------------------------------------------------

TEST_CASE("reports Incomplete for every prefix of a command", "[protocol][incremental]") {
    const std::string input = request({"SET", "foo", "bar"});

    for (std::size_t n = 0; n < input.size(); ++n) {
        Command command;
        const auto result = parse_command(input.substr(0, n), command);

        INFO("prefix length " << n);
        REQUIRE(result.status == ParseStatus::Incomplete);
        REQUIRE(result.consumed == 0);
    }
}

TEST_CASE("a command split across reads parses once complete", "[protocol][incremental]") {
    const std::string input = request({"SET", "key", "value"});
    std::string buffer;
    Command command;

    // Deliver one byte at a time, as a slow or fragmented connection would.
    for (std::size_t i = 0; i + 1 < input.size(); ++i) {
        buffer += input[i];
        REQUIRE(parse_command(buffer, command).status == ParseStatus::Incomplete);
    }

    buffer += input.back();
    const auto result = parse_command(buffer, command);

    REQUIRE(result.ok());
    REQUIRE(command.args == std::vector<std::string>{"SET", "key", "value"});
}

TEST_CASE("empty input is incomplete, not an error", "[protocol][incremental]") {
    Command command;
    REQUIRE(parse_command("", command).status == ParseStatus::Incomplete);
}

// ---------------------------------------------------------------------------
// Resource exhaustion -- lengths are attacker-controlled
// ---------------------------------------------------------------------------

TEST_CASE("a huge declared value size is rejected, not allocated", "[protocol][security]") {
    // The attack: 21 bytes on the wire announcing a 4 GB value. A parser that
    // reserved before checking would be driven out of memory by one packet.
    Command command;
    const std::string attack = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4294967295\r\n";

    const auto result = parse_command(attack, command);

    REQUIRE(result.status == ParseStatus::TooLarge);
    REQUIRE(result.message == "argument exceeds maximum size");
}

TEST_CASE("a huge argument count is rejected", "[protocol][security]") {
    Command command;

    const auto result = parse_command("*1000000000\r\n", command);

    REQUIRE(result.status == ParseStatus::TooLarge);
    REQUIRE(result.message == "too many arguments");
}

TEST_CASE("limits are configurable and enforced", "[protocol][security]") {
    Limits limits;
    limits.max_arg_bytes = 8;
    limits.max_args = 4;
    Command command;

    REQUIRE(parse_command(request({"SET", "k", "123456789"}), command, limits).status ==
            ParseStatus::TooLarge);
    REQUIRE(parse_command("*5\r\n", command, limits).status == ParseStatus::TooLarge);
    // Just within the limits still parses.
    REQUIRE(parse_command(request({"SET", "k", "12345678"}), command, limits).ok());
}

TEST_CASE("an over-long pending request stops being buffered", "[protocol][security]") {
    // Without this a client could dribble bytes forever, never completing a
    // command, while the server's read buffer grew without bound.
    Limits limits;
    limits.max_request_bytes = 64;
    Command command;

    const auto result = parse_command(std::string(65, 'x'), command, limits);

    REQUIRE(result.status == ParseStatus::TooLarge);
    REQUIRE(result.message == "request exceeds maximum size");
}

TEST_CASE("negative lengths are rejected", "[protocol][security]") {
    // A negative length could become a huge value if cast to unsigned.
    Command command;

    REQUIRE(parse_command("*-1\r\n", command).status == ParseStatus::Protocol);
    REQUIRE(parse_command("*1\r\n$-5\r\n", command).status == ParseStatus::Protocol);
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST_CASE("input not starting with '*' is rejected", "[protocol][malformed]") {
    Command command;
    const auto result = parse_command("GET foo\r\n", command);

    REQUIRE(result.status == ParseStatus::Protocol);
    REQUIRE(result.fatal());
}

TEST_CASE("a non-numeric length is rejected", "[protocol][malformed]") {
    Command command;

    REQUIRE(parse_command("*abc\r\n", command).status == ParseStatus::Protocol);
    REQUIRE(parse_command("*1\r\n$xyz\r\n", command).status == ParseStatus::Protocol);
}

TEST_CASE("a length with trailing garbage is rejected", "[protocol][malformed]") {
    // from_chars reports the unconsumed tail, so "12abc" does not become 12.
    Command command;
    REQUIRE(parse_command("*12abc\r\n", command).status == ParseStatus::Protocol);
}

TEST_CASE("an empty length is rejected", "[protocol][malformed]") {
    Command command;
    REQUIRE(parse_command("*\r\n", command).status == ParseStatus::Protocol);
}

TEST_CASE("a bulk string not starting with '$' is rejected", "[protocol][malformed]") {
    Command command;
    REQUIRE(parse_command("*1\r\n+OK\r\n", command).status == ParseStatus::Protocol);
}

TEST_CASE("a bulk string not terminated by CRLF is rejected", "[protocol][malformed]") {
    // Declared length 3 but followed by "barX" rather than "bar\r\n": the
    // stream is out of sync and cannot be trusted.
    Command command;
    const auto result = parse_command("*1\r\n$3\r\nbarXX", command);

    REQUIRE(result.status == ParseStatus::Protocol);
    REQUIRE(result.message == "bulk string not terminated by CRLF");
}

TEST_CASE("malformed input is fatal, incomplete input is not", "[protocol][malformed]") {
    // The server closes on fatal errors; once the byte stream is out of sync
    // there is no reliable way to resynchronise.
    Command command;

    REQUIRE(parse_command("*1\r\n", command).fatal() == false);
    REQUIRE(parse_command("garbage\r\n", command).fatal() == true);
}

// ---------------------------------------------------------------------------
// Response encoding
// ---------------------------------------------------------------------------

TEST_CASE("encodes each reply type", "[protocol][encode]") {
    REQUIRE(encode_simple("OK") == "+OK\r\n");
    REQUIRE(encode_bulk("bar") == "$3\r\nbar\r\n");
    REQUIRE(encode_bulk("") == "$0\r\n\r\n");
    REQUIRE(encode_null() == "$-1\r\n");
    REQUIRE(encode_integer(1) == ":1\r\n");
    REQUIRE(encode_integer(-1) == ":-1\r\n");
    REQUIRE(encode_error("ERR unknown command") == "-ERR unknown command\r\n");
}

TEST_CASE("a null reply is distinct from an empty value", "[protocol][encode]") {
    // GET of a missing key must not look like GET of a key set to "".
    REQUIRE(encode_null() != encode_bulk(""));
}

TEST_CASE("bulk encoding is binary safe", "[protocol][encode]") {
    const std::string value = std::string("a\r\nb\0c", 6);

    REQUIRE(encode_bulk(value) == "$6\r\n" + value + "\r\n");
}

TEST_CASE("newlines in an error message cannot split the response", "[protocol][security]") {
    // If a CRLF survived into an error line, the text after it would be read as
    // the start of another reply -- a response-splitting bug that lets a client
    // forge server replies by choosing a key name.
    const std::string encoded = encode_error("ERR bad\r\n+INJECTED");

    REQUIRE(encoded.find("\r\n") == encoded.size() - 2);
    REQUIRE(encoded == "-ERR bad  +INJECTED\r\n");
}

TEST_CASE("newlines in a simple status cannot split the response", "[protocol][security]") {
    const std::string encoded = encode_simple("OK\r\n+INJECTED");

    REQUIRE(encoded.find("\r\n") == encoded.size() - 2);
}

TEST_CASE("encodes an array", "[protocol][encode]") {
    REQUIRE(encode_array({"a", "bb"}) == "*2\r\n$1\r\na\r\n$2\r\nbb\r\n");
    REQUIRE(encode_array({}) == "*0\r\n");
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST_CASE("encoded arrays parse back to the original arguments", "[protocol]") {
    const std::vector<std::string> args = {
        "SET", "key with spaces", std::string("binary\0\r\n value", 15)};
    Command command;

    const auto result = parse_command(encode_array(args), command);

    REQUIRE(result.ok());
    REQUIRE(command.args == args);
}
