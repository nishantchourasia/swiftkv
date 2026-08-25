#include "swiftkv/client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>

#include "swiftkv/protocol.hpp"

namespace swiftkv {
namespace {

constexpr std::string_view kCrlf = "\r\n";

std::optional<std::int64_t> to_int(std::string_view text) {
    if (text.empty()) {
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

}  // namespace

std::optional<Reply> parse_reply(std::string_view input, std::size_t& consumed) {
    if (input.empty()) {
        return std::nullopt;
    }

    const std::size_t line_end = input.find(kCrlf);
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }

    const char kind = input.front();
    const std::string_view line = input.substr(1, line_end - 1);

    Reply reply;

    switch (kind) {
        case '+':
            reply.type = Reply::Type::Simple;
            reply.text = std::string(line);
            consumed = line_end + kCrlf.size();
            return reply;

        case '-':
            reply.type = Reply::Type::Error;
            reply.text = std::string(line);
            consumed = line_end + kCrlf.size();
            return reply;

        case ':': {
            const auto value = to_int(line);
            if (!value) {
                return std::nullopt;
            }
            reply.type = Reply::Type::Integer;
            reply.integer = *value;
            consumed = line_end + kCrlf.size();
            return reply;
        }

        case '$': {
            const auto length = to_int(line);
            if (!length) {
                return std::nullopt;
            }
            if (*length < 0) {
                reply.type = Reply::Type::Null;
                consumed = line_end + kCrlf.size();
                return reply;
            }
            const std::size_t body = line_end + kCrlf.size();
            const std::size_t size = static_cast<std::size_t>(*length);
            if (input.size() < body + size + kCrlf.size()) {
                return std::nullopt;
            }
            reply.type = Reply::Type::Bulk;
            reply.text = std::string(input.substr(body, size));
            consumed = body + size + kCrlf.size();
            return reply;
        }

        case '*': {
            const auto count = to_int(line);
            if (!count) {
                return std::nullopt;
            }
            if (*count < 0) {
                reply.type = Reply::Type::Null;
                consumed = line_end + kCrlf.size();
                return reply;
            }
            reply.type = Reply::Type::Array;
            std::size_t pos = line_end + kCrlf.size();
            for (std::int64_t i = 0; i < *count; ++i) {
                std::size_t used = 0;
                auto element = parse_reply(input.substr(pos), used);
                if (!element) {
                    return std::nullopt;  // whole array must arrive together
                }
                reply.elements.push_back(std::move(*element));
                pos += used;
            }
            consumed = pos;
            return reply;
        }

        default:
            return std::nullopt;
    }
}

NetResult Client::connect(const std::string& host, std::uint16_t port,
                          std::chrono::milliseconds timeout) {
    disconnect();
    timeout_ = timeout;

    FileDescriptor fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) {
        return NetResult::failure("socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    const std::string target = (host.empty() || host == "localhost") ? "127.0.0.1" : host;
    if (::inet_pton(AF_INET, target.c_str(), &address.sin_addr) != 1) {
        NetResult result;
        result.ok = false;
        result.error = EINVAL;
        result.message = "invalid address: " + host;
        return result;
    }

    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        return NetResult::failure("connect");
    }

    // Bound every subsequent read, so a server that goes silent cannot hang us.
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    set_tcp_nodelay(fd.get());

    fd_ = std::move(fd);
    buffer_.clear();
    return NetResult::success();
}

void Client::disconnect() {
    fd_.reset();
    buffer_.clear();
}

NetResult Client::send_raw(std::string_view bytes) {
    if (!fd_) {
        NetResult result;
        result.ok = false;
        result.error = ENOTCONN;
        result.message = "not connected";
        return result;
    }

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n =
            ::send(fd_.get(), bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return NetResult::failure("send");
    }
    return NetResult::success();
}

std::optional<Reply> Client::read_reply() {
    // Bytes left from a previous read may already hold a complete reply --
    // pipelined replies arrive together.
    while (true) {
        std::size_t consumed = 0;
        if (auto reply = parse_reply(buffer_, consumed)) {
            buffer_.erase(0, consumed);
            return reply;
        }

        if (!fd_) {
            return std::nullopt;
        }

        char chunk[16 * 1024];
        const ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return std::nullopt;  // closed, timed out, or errored
    }
}

std::optional<Reply> Client::command(const std::vector<std::string>& args) {
    if (!send_raw(encode_array(args)).ok) {
        return std::nullopt;
    }
    return read_reply();
}

bool Client::set(const std::string& key, const std::string& value) {
    auto reply = command({"SET", key, value});
    return reply && reply->type == Reply::Type::Simple && reply->text == "OK";
}

std::optional<std::string> Client::get(const std::string& key) {
    auto reply = command({"GET", key});
    if (!reply || reply->type != Reply::Type::Bulk) {
        return std::nullopt;
    }
    return reply->text;
}

std::int64_t Client::del(const std::string& key) {
    auto reply = command({"DEL", key});
    if (!reply || reply->type != Reply::Type::Integer) {
        return 0;
    }
    return reply->integer;
}

bool Client::ping() {
    auto reply = command({"PING"});
    return reply && reply->type == Reply::Type::Simple && reply->text == "PONG";
}

}  // namespace swiftkv
