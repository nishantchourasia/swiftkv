#include "swiftkv/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace swiftkv {

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0) {
        // Retry on EINTR. A close interrupted by a signal has, on Linux,
        // already released the descriptor, but looping here is harmless and
        // keeps the intent explicit.
        while (::close(fd_) == -1 && errno == EINTR) {
        }
    }
    fd_ = fd;
}

NetResult NetResult::failure(const char* what) {
    NetResult result;
    result.ok = false;
    result.error = errno;
    result.message = std::string(what) + ": " + std::strerror(errno);
    return result;
}

NetResult set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return NetResult::failure("fcntl(F_GETFL)");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return NetResult::failure("fcntl(F_SETFL, O_NONBLOCK)");
    }
    return NetResult::success();
}

NetResult set_tcp_nodelay(int fd) {
    const int on = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == -1) {
        return NetResult::failure("setsockopt(TCP_NODELAY)");
    }
    return NetResult::success();
}

FileDescriptor listen_on(const std::string& host, std::uint16_t port, int backlog,
                         NetResult& result) {
    FileDescriptor fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) {
        result = NetResult::failure("socket");
        return {};
    }

    const int on = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        result = NetResult::failure("setsockopt(SO_REUSEADDR)");
        return {};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);

    if (host.empty() || host == "0.0.0.0" || host == "*") {
        address.sin_addr.s_addr = ::htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        result.ok = false;
        result.error = EINVAL;
        result.message = "invalid bind address: " + host;
        return {};
    }

    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        result = NetResult::failure("bind");
        return {};
    }

    if (::listen(fd.get(), backlog) == -1) {
        result = NetResult::failure("listen");
        return {};
    }

    result = set_nonblocking(fd.get());
    if (!result.ok) {
        return {};
    }

    result = NetResult::success();
    return fd;
}

std::uint16_t local_port(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == -1) {
        return 0;
    }
    return ::ntohs(address.sin_port);
}

std::string peer_address(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) == -1) {
        return "unknown";
    }
    char buffer[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        return "unknown";
    }
    return std::string(buffer) + ":" + std::to_string(::ntohs(address.sin_port));
}

}  // namespace swiftkv
