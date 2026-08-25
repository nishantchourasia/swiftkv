#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "swiftkv/net.hpp"

namespace swiftkv {

/// One HTTP response.
struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;

    static HttpResponse text(std::string body);
    static HttpResponse json(std::string body);
    static HttpResponse html(std::string body);
    static HttpResponse not_found();
    static HttpResponse method_not_allowed();
    static HttpResponse service_unavailable(std::string body);
};

/// A deliberately small HTTP server for operational endpoints.
///
/// ### Why HTTP at all, when the data protocol is RESP
///
/// Health checks, metrics scrapers and browsers all speak HTTP and none of them
/// speak RESP. Exposing `/health` over the data protocol would mean every
/// monitoring tool needed a custom client, which is why real databases separate
/// the two.
///
/// ### Why it is a separate port and a separate thread
///
/// Keeping it off the data port means an operator can firewall them
/// independently -- the dashboard can be reachable from a browser while the
/// data port stays closed. Keeping it off the event loops means a slow scraper
/// cannot occupy a loop that is serving thousands of clients.
///
/// ### Why blocking I/O here is the right choice
///
/// The event loops exist because the data port must handle thousands of
/// concurrent connections. This port handles a health check every few seconds
/// and a dashboard poll every second. One thread with blocking accepts is
/// simpler, and simpler is worth more than scalability that will never be
/// exercised. Connections are handled one at a time with a short socket
/// timeout, so a stalled client cannot block the endpoint for long.
///
/// ### Security
///
/// Binds to `127.0.0.1` by default. The endpoints expose operational counts,
/// never key names or values, but they do reveal traffic volume and server
/// health, so they should not be public.
class HttpAdminServer {
public:
    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = 6381;

        /// Reject a request whose headers exceed this. Bounds the read buffer
        /// against a client that never sends a blank line.
        std::size_t max_request_bytes = 16 * 1024;

        /// Socket timeout for reading a request and writing a response.
        int io_timeout_ms = 5'000;
    };

    /// Produces a response for a method and path. Supplied by the owner, so
    /// this class knows nothing about stores or metrics and can be tested on
    /// its own.
    using Handler = std::function<HttpResponse(std::string_view method, std::string_view path)>;

    HttpAdminServer(Config config, Handler handler);
    ~HttpAdminServer();

    HttpAdminServer(const HttpAdminServer&) = delete;
    HttpAdminServer& operator=(const HttpAdminServer&) = delete;

    NetResult start();
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }

private:
    void run();
    void serve(FileDescriptor client);

    Config config_;
    Handler handler_;

    FileDescriptor listener_;
    FileDescriptor wakeup_;
    std::uint16_t bound_port_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
};

/// Parse the method and path from an HTTP request line.
///
/// Returns false if the request is not a well-formed request line. Exposed for
/// testing, and because query strings and fragments must be stripped before a
/// path is compared -- `/health?probe=k8s` is a request for `/health`.
bool parse_request_line(std::string_view request, std::string& method, std::string& path);

/// Escape a string for embedding in JSON.
std::string json_escape(std::string_view text);

}  // namespace swiftkv
