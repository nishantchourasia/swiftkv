#include "swiftkv/admin.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "catch.hpp"
#include "swiftkv/client.hpp"
#include "swiftkv/server.hpp"

using namespace swiftkv;
using namespace std::chrono_literals;

namespace {

/// Minimal HTTP client: send raw bytes, read the whole response.
std::string http_raw(std::uint16_t port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    timeval tv{};
    tv.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }

    ssize_t sent = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    (void)sent;

    std::string response;
    char buffer[8192];
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return response;
}

std::string http_get(std::uint16_t port, const std::string& path) {
    return http_raw(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
}

int status_of(const std::string& response) {
    if (response.rfind("HTTP/1.1 ", 0) != 0) {
        return -1;
    }
    return std::stoi(response.substr(9, 3));
}

std::string body_of(const std::string& response) {
    const std::size_t split = response.find("\r\n\r\n");
    return split == std::string::npos ? std::string{} : response.substr(split + 4);
}

/// An admin server on a kernel-assigned port, stopped on scope exit.
struct TestAdmin {
    HttpAdminServer server;

    explicit TestAdmin(HttpAdminServer::Handler handler)
        : server(make_config(), std::move(handler)) {
        REQUIRE(server.start().ok);
        REQUIRE(server.port() != 0);
    }
    ~TestAdmin() { server.stop(); }

    static HttpAdminServer::Config make_config() {
        HttpAdminServer::Config config;
        config.host = "127.0.0.1";
        config.port = 0;  // kernel picks a free port; no collisions on a shared host
        return config;
    }

    [[nodiscard]] std::uint16_t port() const { return server.port(); }
};

HttpAdminServer::Handler echo_handler() {
    return [](std::string_view method, std::string_view path) {
        return HttpResponse::text(std::string(method) + " " + std::string(path));
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// Request-line parsing
// ---------------------------------------------------------------------------

TEST_CASE("parses a request line", "[admin]") {
    std::string method;
    std::string path;

    REQUIRE(parse_request_line("GET /health HTTP/1.1\r\nHost: x\r\n\r\n", method, path));
    REQUIRE(method == "GET");
    REQUIRE(path == "/health");
}

TEST_CASE("query strings are stripped from the path", "[admin]") {
    // Kubernetes and most scrapers append parameters; keeping them would 404
    // every probe.
    std::string method;
    std::string path;

    REQUIRE(parse_request_line("GET /health?probe=k8s&t=1 HTTP/1.1\r\n\r\n", method, path));
    REQUIRE(path == "/health");
}

TEST_CASE("fragments are stripped from the path", "[admin]") {
    std::string method;
    std::string path;

    REQUIRE(parse_request_line("GET /metrics#section HTTP/1.1\r\n\r\n", method, path));
    REQUIRE(path == "/metrics");
}

TEST_CASE("malformed request lines are rejected", "[admin]") {
    std::string method;
    std::string path;

    REQUIRE_FALSE(parse_request_line("", method, path));
    REQUIRE_FALSE(parse_request_line("GET\r\n\r\n", method, path));
    REQUIRE_FALSE(parse_request_line("GET /health\r\n\r\n", method, path));
    REQUIRE_FALSE(parse_request_line("garbage\r\n", method, path));
}

TEST_CASE("the target and version are validated, not just the spaces", "[admin]") {
    // Regression: checking only for two spaces made "not http at all" parse as
    // method "not", target "http", so garbage was reported as an unknown method
    // (405) rather than a malformed request (400).
    std::string method;
    std::string path;

    REQUIRE_FALSE(parse_request_line("not http at all\r\n\r\n", method, path));
    // Target must be origin-form: this server has no proxy role.
    REQUIRE_FALSE(parse_request_line("GET health HTTP/1.1\r\n\r\n", method, path));
    REQUIRE_FALSE(parse_request_line("GET http://elsewhere/ HTTP/1.1\r\n\r\n", method, path));
    // Version must look like a version.
    REQUIRE_FALSE(parse_request_line("GET /health SPDY/3\r\n\r\n", method, path));

    REQUIRE(parse_request_line("GET /health HTTP/1.0\r\n\r\n", method, path));
    REQUIRE(parse_request_line("GET /health HTTP/1.1\r\n\r\n", method, path));
}

// ---------------------------------------------------------------------------
// JSON escaping
// ---------------------------------------------------------------------------

TEST_CASE("json escaping handles the characters that would break a document",
          "[admin][json]") {
    REQUIRE(json_escape("plain") == "plain");
    REQUIRE(json_escape("say \"hi\"") == "say \\\"hi\\\"");
    REQUIRE(json_escape("back\\slash") == "back\\\\slash");
    REQUIRE(json_escape("line\nbreak") == "line\\nbreak");
    REQUIRE(json_escape("tab\there") == "tab\\there");
    REQUIRE(json_escape(std::string("nul\0byte", 8)) == "nul\\u0000byte");
}

// ---------------------------------------------------------------------------
// HTTP serving
// ---------------------------------------------------------------------------

TEST_CASE("serves a handler response", "[admin]") {
    TestAdmin admin(echo_handler());

    const std::string response = http_get(admin.port(), "/hello");

    REQUIRE(status_of(response) == 200);
    REQUIRE(body_of(response) == "GET /hello");
}

TEST_CASE("sends the correct content length", "[admin]") {
    TestAdmin admin(echo_handler());

    const std::string response = http_get(admin.port(), "/abc");

    REQUIRE(response.find("Content-Length: 8") != std::string::npos);
    REQUIRE(body_of(response).size() == 8);
}

TEST_CASE("responses are marked no-store", "[admin]") {
    // A dashboard polls once a second; a cached response would show stale
    // numbers indefinitely.
    TestAdmin admin(echo_handler());

    REQUIRE(http_get(admin.port(), "/x").find("Cache-Control: no-store") != std::string::npos);
}

TEST_CASE("HEAD returns headers without a body", "[admin]") {
    TestAdmin admin(echo_handler());

    const std::string response =
        http_raw(admin.port(), "HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n");

    REQUIRE(status_of(response) == 200);
    REQUIRE(body_of(response).empty());
    REQUIRE(response.find("Content-Length: 11") != std::string::npos);
}

TEST_CASE("non-GET methods are refused", "[admin][security]") {
    // The endpoint is read-only; accepting writes would be an unauthenticated
    // mutation surface.
    TestAdmin admin(echo_handler());

    for (const char* method : {"POST", "PUT", "DELETE", "PATCH"}) {
        const std::string response =
            http_raw(admin.port(), std::string(method) + " /health HTTP/1.1\r\nHost: x\r\n\r\n");
        INFO("method " << method);
        REQUIRE(status_of(response) == 405);
    }
}

TEST_CASE("a malformed request gets 400, not a crash", "[admin][security]") {
    TestAdmin admin(echo_handler());

    REQUIRE(status_of(http_raw(admin.port(), "not http at all\r\n\r\n")) == 400);
}

TEST_CASE("an oversized request is refused", "[admin][security]") {
    // Bounds the read buffer against a client that never sends a blank line.
    HttpAdminServer::Config config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.max_request_bytes = 1024;
    HttpAdminServer admin(config, echo_handler());
    REQUIRE(admin.start().ok);

    const std::string huge =
        "GET /" + std::string(4096, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n";
    REQUIRE(status_of(http_raw(admin.port(), huge)) == 413);

    admin.stop();
}

TEST_CASE("the server survives a client that disconnects mid-request",
          "[admin][security]") {
    TestAdmin admin(echo_handler());

    for (int i = 0; i < 20; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(admin.port());
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            ssize_t ignored = ::send(fd, "GET /par", 8, MSG_NOSIGNAL);  // no terminator
            (void)ignored;
        }
        ::close(fd);  // vanish
    }

    // Still serving.
    REQUIRE(status_of(http_get(admin.port(), "/still-here")) == 200);
}

TEST_CASE("stop is idempotent", "[admin]") {
    TestAdmin admin(echo_handler());

    admin.server.stop();
    admin.server.stop();

    REQUIRE_FALSE(admin.server.running());
}

TEST_CASE("a server with no handler refuses to start", "[admin]") {
    HttpAdminServer::Config config;
    config.port = 0;
    HttpAdminServer admin(config, nullptr);

    REQUIRE_FALSE(admin.start().ok);
}

TEST_CASE("many sequential requests are served", "[admin]") {
    TestAdmin admin(echo_handler());

    for (int i = 0; i < 100; ++i) {
        REQUIRE(status_of(http_get(admin.port(), "/req")) == 200);
    }
}

// ---------------------------------------------------------------------------
// The endpoints the server actually exposes
// ---------------------------------------------------------------------------

namespace {

/// A real SwiftKV server with the admin endpoint enabled.
struct TestServerWithAdmin {
    Server server;

    TestServerWithAdmin() : server(make_config()) {
        REQUIRE(server.start().ok);
        REQUIRE(server.admin_port() != 0);
    }
    ~TestServerWithAdmin() { server.stop(); }

    static Server::Config make_config() {
        Server::Config config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.io_threads = 2;
        config.admin_enabled = true;
        config.admin_host = "127.0.0.1";
        config.admin_port = 0;
        return config;
    }

    [[nodiscard]] std::uint16_t admin_port() const { return server.admin_port(); }
};

}  // namespace

TEST_CASE("health reports liveness", "[admin][endpoints]") {
    TestServerWithAdmin fixture;

    const std::string response = http_get(fixture.admin_port(), "/health");

    REQUIRE(status_of(response) == 200);
    REQUIRE(body_of(response).find("\"ok\"") != std::string::npos);
}

TEST_CASE("health answers a probe with query parameters", "[admin][endpoints]") {
    TestServerWithAdmin fixture;

    REQUIRE(status_of(http_get(fixture.admin_port(), "/health?probe=k8s")) == 200);
}

TEST_CASE("ready reports readiness separately from liveness", "[admin][endpoints]") {
    // Distinct endpoints so an orchestrator can wait for a slow start instead
    // of restarting the process.
    TestServerWithAdmin fixture;

    const std::string response = http_get(fixture.admin_port(), "/ready");

    REQUIRE(status_of(response) == 200);
    REQUIRE(body_of(response).find("ready") != std::string::npos);
}

TEST_CASE("metrics are exposed in Prometheus format", "[admin][endpoints]") {
    TestServerWithAdmin fixture;
    Client client;
    REQUIRE(client.connect("127.0.0.1", fixture.server.port(), 5s).ok);
    client.set("k", "v");

    const std::string body = body_of(http_get(fixture.admin_port(), "/metrics"));

    REQUIRE(body.find("# TYPE swiftkv_commands_total counter") != std::string::npos);
    REQUIRE(body.find("swiftkv_sets_total 1") != std::string::npos);
    REQUIRE(body.find("swiftkv_command_latency_seconds{quantile=\"0.99\"}") !=
            std::string::npos);
}

TEST_CASE("stats.json reflects real traffic", "[admin][endpoints]") {
    TestServerWithAdmin fixture;
    Client client;
    REQUIRE(client.connect("127.0.0.1", fixture.server.port(), 5s).ok);
    client.set("a", "1");
    client.get("a");
    client.get("missing");

    const std::string body = body_of(http_get(fixture.admin_port(), "/stats.json"));

    REQUIRE(body.find("\"keys\": 1") != std::string::npos);
    REQUIRE(body.find("\"hits\": 1") != std::string::npos);
    REQUIRE(body.find("\"misses\": 1") != std::string::npos);
    REQUIRE(body.find("\"sets\": 1") != std::string::npos);
    REQUIRE(body.find("\"latency_us\"") != std::string::npos);
}

TEST_CASE("stats.json never discloses keys or values", "[admin][endpoints][security]") {
    // The endpoint is meant to be safe to point a browser at. It reports counts
    // and timings; it must not leak what is stored.
    TestServerWithAdmin fixture;
    Client client;
    REQUIRE(client.connect("127.0.0.1", fixture.server.port(), 5s).ok);
    client.set("secret-key-name", "secret-value-content");

    const std::string body = body_of(http_get(fixture.admin_port(), "/stats.json"));

    REQUIRE(body.find("secret-key-name") == std::string::npos);
    REQUIRE(body.find("secret-value-content") == std::string::npos);
}

TEST_CASE("latency percentiles appear once commands have run", "[admin][endpoints]") {
    TestServerWithAdmin fixture;
    Client client;
    REQUIRE(client.connect("127.0.0.1", fixture.server.port(), 5s).ok);
    for (int i = 0; i < 200; ++i) {
        client.set("k" + std::to_string(i), "v");
    }

    const std::string body = body_of(http_get(fixture.admin_port(), "/stats.json"));

    REQUIRE(body.find("\"p50\"") != std::string::npos);
    REQUIRE(body.find("\"p99\"") != std::string::npos);
    // At least the 200 sets plus the connection's own traffic were timed.
    REQUIRE(body.find("\"count\": 0,") == std::string::npos);
}

TEST_CASE("the dashboard is served at the root", "[admin][endpoints]") {
    TestServerWithAdmin fixture;

    const std::string response = http_get(fixture.admin_port(), "/");

    REQUIRE(status_of(response) == 200);
    REQUIRE(response.find("text/html") != std::string::npos);
    REQUIRE(body_of(response).find("SwiftKV") != std::string::npos);
    REQUIRE(body_of(response).find("stats.json") != std::string::npos);
}

TEST_CASE("the dashboard contains no hardcoded metric values",
          "[admin][endpoints][honesty]") {
    // Every figure must come from /stats.json. Before the first poll returns,
    // fields read as an em dash rather than a plausible-looking number.
    TestServerWithAdmin fixture;

    const std::string page = body_of(http_get(fixture.admin_port(), "/"));

    REQUIRE(page.find("fetch('stats.json'") != std::string::npos);
    // The placeholder used before real data arrives.
    REQUIRE(page.find("&mdash;") != std::string::npos);
}

TEST_CASE("unknown paths return 404", "[admin][endpoints]") {
    TestServerWithAdmin fixture;

    REQUIRE(status_of(http_get(fixture.admin_port(), "/nope")) == 404);
    REQUIRE(status_of(http_get(fixture.admin_port(), "/../etc/passwd")) == 404);
}

TEST_CASE("the admin endpoint is off unless asked for", "[admin][endpoints]") {
    // A process should not open a port nobody requested.
    Server::Config config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.io_threads = 1;
    Server server(config);
    REQUIRE(server.start().ok);

    REQUIRE(server.admin_port() == 0);

    server.stop();
}

TEST_CASE("the data port keeps working while the dashboard is polled",
          "[admin][endpoints]") {
    TestServerWithAdmin fixture;
    Client client;
    REQUIRE(client.connect("127.0.0.1", fixture.server.port(), 5s).ok);

    for (int i = 0; i < 25; ++i) {
        REQUIRE(client.set("k" + std::to_string(i), "v"));
        REQUIRE(status_of(http_get(fixture.admin_port(), "/stats.json")) == 200);
    }

    REQUIRE(client.ping());
}
