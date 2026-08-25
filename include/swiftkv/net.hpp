#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace swiftkv {

/// Owns a file descriptor and closes it exactly once.
///
/// Every socket in this server is held by one of these. A raw `int` descriptor
/// leaks on any early return or exception, and a leaked descriptor is not a
/// slow leak like memory -- the process hits its `RLIMIT_NOFILE` and then
/// cannot accept *any* connection. Making ownership explicit removes a whole
/// class of outage.
///
/// Move-only, for the same reason `unique_ptr` is: two owners would close the
/// same descriptor twice, and by then the number may have been reused by an
/// unrelated connection, so the second close would sever a live client.
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    /// Relinquish ownership without closing.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    /// Close the descriptor, if any.
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

/// Result of a network operation that may fail.
struct NetResult {
    bool ok = false;
    int error = 0;  ///< errno value when !ok
    std::string message;

    static NetResult success() { return {true, 0, {}}; }
    static NetResult failure(const char* what);
};

/// Put a descriptor into non-blocking mode.
///
/// Required by the event loop: a blocking `read` on one connection would stall
/// the thread and with it every other connection that thread is serving.
NetResult set_nonblocking(int fd);

/// Disable Nagle's algorithm.
///
/// Nagle delays a small packet hoping to coalesce it with the next one. For a
/// request/response store that is exactly wrong -- the reply is the last thing
/// to send, so there is nothing to coalesce with, and the delay lands directly
/// in the client's measured latency as tens of milliseconds.
NetResult set_tcp_nodelay(int fd);

/// Create a listening socket bound to `host`:`port`.
///
/// Sets SO_REUSEADDR so a restarted server can bind immediately rather than
/// waiting out the TIME_WAIT state of its previous sockets -- otherwise every
/// restart fails for up to a couple of minutes.
FileDescriptor listen_on(const std::string& host, std::uint16_t port, int backlog,
                         NetResult& result);

/// The port a listening socket is actually bound to.
///
/// Needed when binding to port 0, which asks the kernel to choose a free port.
/// Tests use that so they never collide with another process on a shared machine.
std::uint16_t local_port(int fd);

/// Describe a connected peer as "host:port", for logging.
std::string peer_address(int fd);

}  // namespace swiftkv
