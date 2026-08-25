#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "swiftkv/net.hpp"

namespace swiftkv {

/// A decoded server reply.
struct Reply {
    enum class Type { Simple, Error, Integer, Bulk, Null, Array };

    Type type = Type::Null;
    std::string text;             ///< Simple, Error and Bulk payloads
    std::int64_t integer = 0;     ///< Integer payload
    std::vector<Reply> elements;  ///< Array payload

    [[nodiscard]] bool is_error() const noexcept { return type == Type::Error; }
    [[nodiscard]] bool is_null() const noexcept { return type == Type::Null; }
};

/// Decode one reply from the front of `input`.
///
/// Returns nullopt if more bytes are needed. `consumed` is set to the number of
/// bytes used on success.
std::optional<Reply> parse_reply(std::string_view input, std::size_t& consumed);

/// A blocking client, used by the CLI, the integration tests and the benchmark.
///
/// Blocking rather than event-driven on purpose. A client issues one command
/// and waits for its answer, so there is nothing to overlap; blocking I/O makes
/// the code obvious and, importantly for the benchmark, makes each measured
/// latency the true round-trip time rather than an artefact of a scheduler.
class Client {
public:
    Client() = default;
    ~Client() = default;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;

    /// Connect to a server. A timeout is applied to the connect itself and to
    /// every later read: without one, a server that accepts and then stops
    /// responding would hang the client forever.
    NetResult connect(const std::string& host, std::uint16_t port,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5));

    void disconnect();
    [[nodiscard]] bool connected() const noexcept { return fd_.valid(); }

    /// Send a command and wait for its reply.
    std::optional<Reply> command(const std::vector<std::string>& args);

    /// Send raw bytes, for tests that need to drive the protocol directly
    /// (malformed input, partial commands).
    NetResult send_raw(std::string_view bytes);

    /// Read one reply, using any bytes left over from a previous read.
    std::optional<Reply> read_reply();

    // Convenience wrappers.
    bool set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    std::int64_t del(const std::string& key);
    bool ping();

private:
    FileDescriptor fd_;
    std::string buffer_;  ///< bytes read but not yet consumed by a reply
    std::chrono::milliseconds timeout_{5000};
};

}  // namespace swiftkv
