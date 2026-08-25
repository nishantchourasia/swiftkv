#include "swiftkv/admin.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <sstream>

namespace swiftkv {
namespace {

constexpr int kPollTimeoutMs = 300;

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

}  // namespace

HttpResponse HttpResponse::text(std::string body) {
    return {200, "text/plain; charset=utf-8", std::move(body)};
}

HttpResponse HttpResponse::json(std::string body) {
    return {200, "application/json; charset=utf-8", std::move(body)};
}

HttpResponse HttpResponse::html(std::string body) {
    return {200, "text/html; charset=utf-8", std::move(body)};
}

HttpResponse HttpResponse::not_found() {
    return {404, "text/plain; charset=utf-8", "not found\n"};
}

HttpResponse HttpResponse::method_not_allowed() {
    return {405, "text/plain; charset=utf-8", "method not allowed\n"};
}

HttpResponse HttpResponse::service_unavailable(std::string body) {
    return {503, "application/json; charset=utf-8", std::move(body)};
}

bool parse_request_line(std::string_view request, std::string& method, std::string& path) {
    const std::size_t line_end = request.find("\r\n");
    const std::string_view line =
        line_end == std::string_view::npos ? request : request.substr(0, line_end);

    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return false;
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return false;
    }

    method = std::string(line.substr(0, first_space));
    std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = line.substr(second_space + 1);

    if (method.empty() || target.empty()) {
        return false;
    }

    // Validate the target and version, not merely the presence of two spaces.
    //
    // Without these checks a line like "not http at all" parses as method
    // "not", target "http" -- so plain garbage would be reported as an unknown
    // *method* (405) instead of a malformed *request* (400). Worse, it means
    // arbitrary text is being interpreted as a request rather than rejected.
    //
    // Only origin-form targets are accepted, because this server has no proxy
    // role and every path it serves begins with '/'.
    if (target.front() != '/') {
        return false;
    }
    if (version.rfind("HTTP/", 0) != 0) {
        return false;
    }

    // A path must be compared without its query string or fragment:
    // "/health?probe=k8s" is a request for "/health". Kubernetes and most
    // scrapers append parameters, so ignoring this would 404 every probe.
    const std::size_t cut = target.find_first_of("?#");
    if (cut != std::string_view::npos) {
        target = target.substr(0, cut);
    }

    path = std::string(target);
    return true;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

HttpAdminServer::HttpAdminServer(Config config, Handler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {}

HttpAdminServer::~HttpAdminServer() { stop(); }

NetResult HttpAdminServer::start() {
    if (running_.load()) {
        return NetResult::success();
    }
    if (!handler_) {
        NetResult result;
        result.ok = false;
        result.error = EINVAL;
        result.message = "admin server requires a handler";
        return result;
    }

    NetResult result;
    listener_ = listen_on(config_.host, config_.port, 64, result);
    if (!result.ok) {
        return result;
    }
    bound_port_ = local_port(listener_.get());

    wakeup_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wakeup_) {
        listener_.reset();
        return NetResult::failure("eventfd(admin)");
    }

    running_.store(true);
    stopping_.store(false);
    thread_ = std::thread([this] { run(); });
    return NetResult::success();
}

void HttpAdminServer::stop() {
    if (!running_.exchange(false)) {
        listener_.reset();
        wakeup_.reset();
        return;
    }

    stopping_.store(true);
    if (wakeup_) {
        const std::uint64_t one = 1;
        ssize_t ignored = ::write(wakeup_.get(), &one, sizeof(one));
        (void)ignored;
    }

    // Join before closing the listener. The serving thread is the only one that
    // touches it, and closing a descriptor another thread is using is a race --
    // the same mistake ThreadSanitizer caught in the data-plane server.
    if (thread_.joinable()) {
        thread_.join();
    }
    listener_.reset();
    wakeup_.reset();
}

void HttpAdminServer::run() {
    FileDescriptor poller(::epoll_create1(EPOLL_CLOEXEC));
    if (!poller) {
        return;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listener_.get();
    if (::epoll_ctl(poller.get(), EPOLL_CTL_ADD, listener_.get(), &event) == -1) {
        return;
    }
    event.data.fd = wakeup_.get();
    if (::epoll_ctl(poller.get(), EPOLL_CTL_ADD, wakeup_.get(), &event) == -1) {
        return;
    }

    while (!stopping_.load()) {
        epoll_event ready[2];
        const int count = ::epoll_wait(poller.get(), ready, 2, kPollTimeoutMs);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < count; ++i) {
            if (ready[i].data.fd == wakeup_.get()) {
                std::uint64_t drained = 0;
                ssize_t ignored = ::read(wakeup_.get(), &drained, sizeof(drained));
                (void)ignored;
                continue;
            }

            while (true) {
                const int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0) {
                    break;  // drained, or an error we simply skip
                }
                serve(FileDescriptor(fd));
            }
        }
    }
}

void HttpAdminServer::serve(FileDescriptor client) {
    // A short timeout on both directions: this endpoint serves one request at a
    // time, so a client that connects and then says nothing must not be able to
    // hold it hostage.
    timeval tv{};
    tv.tv_sec = config_.io_timeout_ms / 1000;
    tv.tv_usec = (config_.io_timeout_ms % 1000) * 1000;
    ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string request;
    char chunk[4096];
    bool oversized = false;

    // Read until the blank line that ends the headers. The body is ignored:
    // every endpoint here is a GET.
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(client.get(), chunk, sizeof(chunk), 0);
        if (n > 0) {
            request.append(chunk, static_cast<std::size_t>(n));
            if (request.size() > config_.max_request_bytes) {
                oversized = true;
                break;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;  // closed, timed out, or errored
    }

    HttpResponse response;
    std::string method;
    std::string path;

    if (oversized) {
        response = {413, "text/plain; charset=utf-8", "request too large\n"};
    } else if (!parse_request_line(request, method, path)) {
        response = {400, "text/plain; charset=utf-8", "bad request\n"};
    } else if (method != "GET" && method != "HEAD") {
        response = HttpResponse::method_not_allowed();
    } else {
        response = handler_(method, path);
    }

    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\n"
         << "Content-Type: " << response.content_type << "\r\n"
         << "Content-Length: " << response.body.size() << "\r\n"
         // The endpoint is polled repeatedly by dashboards and scrapers; a
         // cached response would show stale numbers forever.
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n"
         << "\r\n";

    std::string out = head.str();
    if (method != "HEAD") {
        out += response.body;
    }

    std::size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t n =
            ::send(client.get(), out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

}  // namespace swiftkv
