#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swiftkv {

/// Wire protocol: a subset of RESP, the protocol Redis speaks.
///
/// RESP was chosen over inventing something because it is length-prefixed and
/// therefore binary-safe -- a value may contain spaces, newlines or NUL bytes
/// without escaping -- and because it is a real protocol with real clients,
/// rather than a toy.
///
/// A request is an array of bulk strings:
///
///     *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
///     ^     ^                                        3 arguments follow
///           ^                                        each: $<byte count>\r\n<bytes>\r\n
///
/// Responses are one of:
///
///     +OK\r\n           simple string
///     $3\r\nbar\r\n     bulk string
///     $-1\r\n           null (key absent)
///     :1\r\n            integer
///     -ERR message\r\n  error
///
/// ### Parsing is incremental, because TCP is a byte stream
///
/// TCP does not preserve message boundaries. A single `read()` may return half
/// a command, or three commands and a fragment of a fourth. The parser is
/// therefore written to report `Incomplete` and consume nothing, so the caller
/// can retain the partial bytes and try again after the next read. A parser
/// that assumed one read equals one command would fail intermittently under
/// exactly the load it most needs to survive.
///
/// ### Limits are enforced before allocation
///
/// The length prefix is attacker-controlled. `$4294967295\r\n` announces a 4 GB
/// value; a parser that reserved that much before checking would be trivially
/// driven out of memory by a single short packet. Every length is validated
/// against `Limits` *before* any buffer is sized.

struct Command {
    std::vector<std::string> args;

    [[nodiscard]] bool empty() const noexcept { return args.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return args.size(); }

    /// The command verb, uppercased. Empty if there are no arguments.
    [[nodiscard]] std::string verb() const;

    void clear() noexcept { args.clear(); }
};

enum class ParseStatus {
    /// A complete command was parsed.
    Ok,
    /// Not enough bytes yet; retain them and read more.
    Incomplete,
    /// Malformed input. The connection should be closed: once the byte stream
    /// is out of sync there is no reliable way to resynchronise.
    Protocol,
    /// A declared length exceeded a configured limit.
    TooLarge,
};

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;

    /// Bytes consumed from the input. Zero unless status is Ok.
    std::size_t consumed = 0;

    /// Human-readable reason, for Protocol and TooLarge. Safe to send to the
    /// client: it describes the protocol violation and never leaks server state.
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return status == ParseStatus::Ok; }
    [[nodiscard]] bool fatal() const noexcept {
        return status == ParseStatus::Protocol || status == ParseStatus::TooLarge;
    }
};

/// Bounds that keep a hostile client from exhausting server memory.
struct Limits {
    /// Maximum arguments in one command. SET needs 3; a request announcing a
    /// million is not a real client.
    std::size_t max_args = 1024;

    /// Maximum bytes in one argument (key or value).
    std::size_t max_arg_bytes = 8u * 1024 * 1024;  // 8 MiB

    /// Maximum bytes the parser will hold while waiting for a command to
    /// complete. Bounds the per-connection read buffer, so a client cannot
    /// force unbounded growth by dribbling out a huge command.
    std::size_t max_request_bytes = 16u * 1024 * 1024;  // 16 MiB
};

/// Parse one command from the front of `input`.
///
/// On `Ok`, `out` holds the command and `result.consumed` says how many bytes
/// to discard. On `Incomplete`, nothing is consumed and `out` is unspecified.
ParseResult parse_command(std::string_view input, Command& out, const Limits& limits = {});

// ---------------------------------------------------------------------------
// Response encoding
// ---------------------------------------------------------------------------

/// `+OK\r\n` -- a short status that contains no newlines.
std::string encode_simple(std::string_view status);

/// `$<n>\r\n<bytes>\r\n` -- binary-safe value.
std::string encode_bulk(std::string_view value);

/// `$-1\r\n` -- the key does not exist. Distinct from an empty value.
std::string encode_null();

/// `:<n>\r\n`
std::string encode_integer(std::int64_t value);

/// `-<message>\r\n`
///
/// Newlines in `message` are replaced, because an embedded CRLF would let the
/// error text be read as the start of another reply and desynchronise the
/// stream -- a response-splitting bug.
std::string encode_error(std::string_view message);

/// `*<n>\r\n` followed by each element as a bulk string.
std::string encode_array(const std::vector<std::string>& values);

}  // namespace swiftkv
