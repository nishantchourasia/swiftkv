#include "swiftkv/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>

namespace swiftkv {
namespace {

constexpr std::string_view kCrlf = "\r\n";

/// Find the CRLF terminating a line starting at `pos`.
std::optional<std::size_t> find_crlf(std::string_view input, std::size_t pos) {
    const std::size_t found = input.find(kCrlf, pos);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    return found;
}

/// Parse a signed integer occupying exactly `text`.
///
/// from_chars is used rather than atoll or stoll because it does not throw, does
/// not allocate, does not consult the locale, and reports trailing garbage --
/// so "12abc" and "" are rejected rather than silently accepted as 12 and 0.
std::optional<std::int64_t> parse_int(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    // from_chars accepts a leading '-' but not '+'; reject '+' for consistency
    // with RESP, which never emits it.
    if (text.front() == '+') {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

ParseResult protocol_error(std::string message) {
    ParseResult result;
    result.status = ParseStatus::Protocol;
    result.message = std::move(message);
    return result;
}

ParseResult too_large(std::string message) {
    ParseResult result;
    result.status = ParseStatus::TooLarge;
    result.message = std::move(message);
    return result;
}

ParseResult incomplete() {
    ParseResult result;
    result.status = ParseStatus::Incomplete;
    return result;
}

}  // namespace

std::string Command::verb() const {
    if (args.empty()) {
        return {};
    }
    std::string out = args.front();
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

ParseResult parse_command(std::string_view input, Command& out, const Limits& limits) {
    // Refuse to keep buffering once a single pending command has grown past the
    // limit. Without this a client could dribble bytes forever and the read
    // buffer would grow without bound even though no command ever completes.
    if (input.size() > limits.max_request_bytes) {
        return too_large("request exceeds maximum size");
    }

    if (input.empty()) {
        return incomplete();
    }

    if (input.front() != '*') {
        // Only arrays of bulk strings are accepted. Redis also supports an
        // "inline" form for humans typing at a socket; it is deliberately not
        // supported here, because it is not length-prefixed and so is not
        // binary-safe.
        return protocol_error("expected '*' at start of command");
    }

    const auto header_end = find_crlf(input, 0);
    if (!header_end) {
        return incomplete();
    }

    const auto count = parse_int(input.substr(1, *header_end - 1));
    if (!count) {
        return protocol_error("invalid multibulk length");
    }
    if (*count < 0) {
        return protocol_error("negative multibulk length");
    }
    if (static_cast<std::size_t>(*count) > limits.max_args) {
        return too_large("too many arguments");
    }

    Command command;
    command.args.reserve(static_cast<std::size_t>(*count));

    std::size_t pos = *header_end + kCrlf.size();

    for (std::int64_t i = 0; i < *count; ++i) {
        if (pos >= input.size()) {
            return incomplete();
        }
        if (input[pos] != '$') {
            return protocol_error("expected '$' at start of bulk string");
        }

        const auto len_end = find_crlf(input, pos);
        if (!len_end) {
            return incomplete();
        }

        const auto length = parse_int(input.substr(pos + 1, *len_end - pos - 1));
        if (!length) {
            return protocol_error("invalid bulk length");
        }
        if (*length < 0) {
            return protocol_error("negative bulk length");
        }

        // The critical check. This length is attacker-controlled, and it is
        // validated here -- before it is used to size or index anything.
        if (static_cast<std::uint64_t>(*length) > limits.max_arg_bytes) {
            return too_large("argument exceeds maximum size");
        }

        const std::size_t body = *len_end + kCrlf.size();
        const std::size_t size = static_cast<std::size_t>(*length);

        // Need the payload plus its trailing CRLF.
        if (input.size() < body + size + kCrlf.size()) {
            return incomplete();
        }
        if (input.substr(body + size, kCrlf.size()) != kCrlf) {
            return protocol_error("bulk string not terminated by CRLF");
        }

        command.args.emplace_back(input.substr(body, size));
        pos = body + size + kCrlf.size();
    }

    out = std::move(command);

    ParseResult result;
    result.status = ParseStatus::Ok;
    result.consumed = pos;
    return result;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

namespace {

/// Replace CR and LF so a value cannot terminate its own reply early.
std::string sanitise_line(std::string_view text) {
    std::string out(text);
    std::replace(out.begin(), out.end(), '\r', ' ');
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

}  // namespace

std::string encode_simple(std::string_view status) {
    return "+" + sanitise_line(status) + std::string(kCrlf);
}

std::string encode_bulk(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 16);
    out += '$';
    out += std::to_string(value.size());
    out += kCrlf;
    out += value;
    out += kCrlf;
    return out;
}

std::string encode_null() { return "$-1" + std::string(kCrlf); }

std::string encode_integer(std::int64_t value) {
    return ":" + std::to_string(value) + std::string(kCrlf);
}

std::string encode_error(std::string_view message) {
    return "-" + sanitise_line(message) + std::string(kCrlf);
}

std::string encode_array(const std::vector<std::string>& values) {
    std::string out = "*" + std::to_string(values.size()) + std::string(kCrlf);
    for (const auto& value : values) {
        out += encode_bulk(value);
    }
    return out;
}

}  // namespace swiftkv
