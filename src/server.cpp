#include "swiftkv/server.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace swiftkv {
namespace {

constexpr std::size_t kReadChunk = 64 * 1024;
constexpr int kPollTimeoutMs = 500;  // bounds how long a sweep or stop waits

}  // namespace

Server::Server(Config config)
    : config_(std::move(config)),
      store_(config_.store),
      executor_(store_, metrics_) {
    if (config_.io_threads == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        config_.io_threads = hardware > 0 ? hardware : 4;
    }
    // More loops than connections is pure overhead, and a machine with 512
    // cores would otherwise spawn 512 threads for a toy workload.
    config_.io_threads = std::min<std::size_t>(config_.io_threads, 64);
}

Server::~Server() { stop(); }

NetResult Server::start() {
    if (running_.load()) {
        return NetResult::success();
    }

    // Persistence is set up before the listener binds. Replaying into a store
    // that is already serving clients would race recovered writes against live
    // ones, and a recovered value could silently overwrite a newer one.
    if (!config_.aof_path.empty()) {
        AppendOnlyLog::Config log_config;
        log_config.path = config_.aof_path;
        log_config.sync = config_.aof_sync;
        log_ = std::make_unique<AppendOnlyLog>(log_config);

        replay_result_ = log_->replay(store_);
        if (!replay_result_.ok) {
            // Refuse to start on a corrupt log rather than serving a silently
            // incomplete dataset. An operator can inspect or move the file.
            NetResult failure;
            failure.ok = false;
            failure.error = EINVAL;
            failure.message = "append-only log could not be replayed: " + replay_result_.message;
            log_.reset();
            return failure;
        }

        const NetResult opened = log_->open();
        if (!opened.ok) {
            log_.reset();
            return opened;
        }
        executor_.set_log(log_.get());
    }

    NetResult result;
    listener_ = listen_on(config_.host, config_.port, config_.backlog, result);
    if (!result.ok) {
        return result;
    }
    bound_port_ = local_port(listener_.get());

    // The acceptor waits on its listener through a persistent epoll instance
    // that also watches an eventfd, so stop() can interrupt it immediately
    // instead of waiting out a poll timeout.
    acceptor_epoll_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!acceptor_epoll_) {
        stop();
        return NetResult::failure("epoll_create1(acceptor)");
    }
    acceptor_wakeup_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!acceptor_wakeup_) {
        stop();
        return NetResult::failure("eventfd(acceptor)");
    }
    {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = listener_.get();
        if (::epoll_ctl(acceptor_epoll_.get(), EPOLL_CTL_ADD, listener_.get(), &event) == -1) {
            stop();
            return NetResult::failure("epoll_ctl(listener)");
        }
        event.data.fd = acceptor_wakeup_.get();
        if (::epoll_ctl(acceptor_epoll_.get(), EPOLL_CTL_ADD, acceptor_wakeup_.get(), &event) ==
            -1) {
            stop();
            return NetResult::failure("epoll_ctl(acceptor wakeup)");
        }
    }

    for (std::size_t i = 0; i < config_.io_threads; ++i) {
        auto loop = std::make_unique<Loop>();

        loop->epoll.reset(::epoll_create1(EPOLL_CLOEXEC));
        if (!loop->epoll) {
            stop();
            return NetResult::failure("epoll_create1");
        }

        // An eventfd lets the acceptor (and stop()) interrupt a loop that is
        // blocked in epoll_wait. Without it, a loop would not notice a new
        // connection or a shutdown request until its poll timeout expired.
        loop->wakeup.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (!loop->wakeup) {
            stop();
            return NetResult::failure("eventfd");
        }

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = loop->wakeup.get();
        if (::epoll_ctl(loop->epoll.get(), EPOLL_CTL_ADD, loop->wakeup.get(), &event) == -1) {
            stop();
            return NetResult::failure("epoll_ctl(wakeup)");
        }

        loops_.push_back(std::move(loop));
    }

    running_.store(true);
    stopping_.store(false);

    for (auto& loop : loops_) {
        loop->thread = std::thread([this, raw = loop.get()] { run_loop(*raw); });
    }
    acceptor_ = std::thread([this] { run_acceptor(); });

    return NetResult::success();
}

void Server::stop() {
    if (!running_.exchange(false)) {
        // Still clear structures in case start() failed part-way through.
        loops_.clear();
        listener_.reset();
        if (log_) {
            executor_.set_log(nullptr);
            log_->close();
            log_.reset();
        }
        return;
    }

    stopping_.store(true);

    // Wake every thread so none waits out its poll timeout.
    if (acceptor_wakeup_) {
        const std::uint64_t one = 1;
        ssize_t ignored = ::write(acceptor_wakeup_.get(), &one, sizeof(one));
        (void)ignored;
    }
    for (auto& loop : loops_) {
        wake(*loop);
    }

    // Join the acceptor BEFORE closing the listener. The acceptor is the only
    // thread that touches listener_, so closing it from here while that thread
    // still runs is a data race -- and worse, the descriptor number could be
    // reused by a freshly accepted client, leaving the acceptor calling accept
    // on a client socket. ThreadSanitizer caught this.
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    listener_.reset();
    acceptor_epoll_.reset();
    acceptor_wakeup_.reset();

    for (auto& loop : loops_) {
        if (loop->thread.joinable()) {
            loop->thread.join();
        }
    }

    loops_.clear();

    // Close the log only after every loop has stopped, so no thread can append
    // to a log that is being torn down. close() flushes and fsyncs, so a clean
    // shutdown loses nothing regardless of sync policy.
    if (log_) {
        executor_.set_log(nullptr);
        log_->close();
        log_.reset();
    }
}

void Server::wake(Loop& loop) {
    const std::uint64_t one = 1;
    ssize_t ignored = ::write(loop.wakeup.get(), &one, sizeof(one));
    (void)ignored;
}

// ---------------------------------------------------------------------------
// Acceptor
// ---------------------------------------------------------------------------

void Server::run_acceptor() {
    while (!stopping_.load()) {
        const int fd = ::accept4(listener_.get(), nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Nothing pending. Sleep in epoll until the listener becomes
                // readable or stop() writes to the wakeup eventfd. The epoll
                // instance is created once in start(), not per iteration.
                epoll_event ready[2];
                const int count =
                    ::epoll_wait(acceptor_epoll_.get(), ready, 2, kPollTimeoutMs);
                for (int i = 0; i < count; ++i) {
                    if (ready[i].data.fd == acceptor_wakeup_.get()) {
                        std::uint64_t drained = 0;
                        ssize_t ignored =
                            ::read(acceptor_wakeup_.get(), &drained, sizeof(drained));
                        (void)ignored;
                    }
                }
                continue;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                // Out of descriptors. Sleeping briefly avoids a hot loop that
                // would burn a core while the condition persists.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            break;
        }

        FileDescriptor client(fd);

        const std::size_t current = metrics_.connections_current.load(std::memory_order_relaxed);
        if (current >= config_.max_connections) {
            // Count the decision before acting on it, so the counter is already
            // visible by the time the client can observe the refusal. Counting
            // afterwards left a window in which a client had been told it was
            // rejected while the metric still read zero.
            metrics_.bump(metrics_.connections_rejected);

            // Refuse politely and close. Accepting and closing is better than
            // not accepting: the client learns immediately instead of hanging
            // in the backlog, and the descriptor is released at once.
            const std::string refusal = encode_error("ERR max number of clients reached");
            ssize_t ignored = ::send(client.get(), refusal.data(), refusal.size(), MSG_NOSIGNAL);
            (void)ignored;
            continue;
        }

        set_tcp_nodelay(client.get());

        // Hand the descriptor to a loop, round-robin. Each loop then owns it
        // exclusively, so no further synchronisation is needed for its state.
        const std::size_t index =
            next_loop_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
        Loop& loop = *loops_[index];

        {
            std::lock_guard<std::mutex> lock(loop.handoff_mutex);
            loop.handoff.push_back(client.release());
        }
        metrics_.bump(metrics_.connections_accepted);
        metrics_.connections_current.fetch_add(1, std::memory_order_relaxed);
        wake(loop);
    }
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

void Server::run_loop(Loop& loop) {
    std::vector<epoll_event> events(256);

    while (!stopping_.load()) {
        const int count =
            ::epoll_wait(loop.epoll.get(), events.data(), static_cast<int>(events.size()),
                         kPollTimeoutMs);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < count; ++i) {
            const auto& event = events[static_cast<std::size_t>(i)];
            const int fd = event.data.fd;

            if (fd == loop.wakeup.get()) {
                std::uint64_t drained = 0;
                ssize_t ignored = ::read(loop.wakeup.get(), &drained, sizeof(drained));
                (void)ignored;
                continue;
            }

            auto it = loop.connections.find(fd);
            if (it == loop.connections.end()) {
                continue;
            }
            Connection& connection = *it->second;

            if (event.events & (EPOLLHUP | EPOLLERR)) {
                close_connection(loop, fd);
                continue;
            }
            if (event.events & EPOLLIN) {
                handle_readable(loop, connection);
                // handle_readable may have closed the connection.
                if (loop.connections.find(fd) == loop.connections.end()) {
                    continue;
                }
            }
            if (event.events & EPOLLOUT) {
                handle_writable(loop, connection);
            }
        }

        adopt_pending(loop);
        sweep_idle(loop);
    }

    // Shutting down: drop every connection this loop owns.
    for (auto& [fd, connection] : loop.connections) {
        (void)fd;
        (void)connection;
        metrics_.connections_current.fetch_sub(1, std::memory_order_relaxed);
    }
    loop.connections.clear();
}

void Server::adopt_pending(Loop& loop) {
    std::vector<int> pending;
    {
        std::lock_guard<std::mutex> lock(loop.handoff_mutex);
        pending.swap(loop.handoff);
    }

    for (const int fd : pending) {
        auto connection = std::make_unique<Connection>(FileDescriptor(fd));

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = fd;
        if (::epoll_ctl(loop.epoll.get(), EPOLL_CTL_ADD, fd, &event) == -1) {
            metrics_.connections_current.fetch_sub(1, std::memory_order_relaxed);
            continue;  // connection destructor closes the descriptor
        }

        loop.connections.emplace(fd, std::move(connection));
    }
}

void Server::handle_readable(Loop& loop, Connection& connection) {
    const int fd = connection.fd.get();
    char buffer[kReadChunk];

    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            connection.inbox.append(buffer, static_cast<std::size_t>(n));
            metrics_.bump(metrics_.bytes_read, static_cast<std::uint64_t>(n));

            // Bound the buffer. Without this a client could stream bytes that
            // never form a complete command and grow this string without limit.
            if (connection.inbox.size() > config_.limits.max_request_bytes) {
                connection.outbox += encode_error("ERR request exceeds maximum size");
                connection.close_after_write = true;
                break;
            }
            connection.last_active = std::chrono::steady_clock::now();
            continue;
        }

        if (n == 0) {
            close_connection(loop, fd);  // orderly shutdown by the peer
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  // drained for now
        }
        if (errno == EINTR) {
            continue;
        }
        close_connection(loop, fd);
        return;
    }

    if (!connection.close_after_write) {
        process_inbox(connection);
    }
    flush(loop, connection);
}

void Server::process_inbox(Connection& connection) {
    // A single read can carry many pipelined commands; keep parsing until the
    // buffer holds only a partial one.
    while (true) {
        Command command;
        const ParseResult result = parse_command(connection.inbox, command, config_.limits);

        if (result.status == ParseStatus::Incomplete) {
            break;
        }

        if (result.fatal()) {
            // The byte stream is out of sync and cannot be resynchronised, so
            // reply once and close rather than guessing where the next command
            // begins.
            connection.outbox += encode_error("ERR protocol error: " + result.message);
            connection.close_after_write = true;
            metrics_.bump(metrics_.errors);
            connection.inbox.clear();
            break;
        }

        connection.inbox.erase(0, result.consumed);

        const CommandResult executed = executor_.execute(command);
        connection.outbox += executed.reply;

        if (executed.disposition == Disposition::CloseAfterReply) {
            connection.close_after_write = true;
            break;
        }
    }
}

void Server::flush(Loop& loop, Connection& connection) {
    const int fd = connection.fd.get();

    while (connection.written < connection.outbox.size()) {
        const std::size_t remaining = connection.outbox.size() - connection.written;
        // MSG_NOSIGNAL: without it, writing to a socket the peer has closed
        // raises SIGPIPE and kills the whole process. Here it returns EPIPE
        // and only this connection is affected.
        const ssize_t n = ::send(fd, connection.outbox.data() + connection.written, remaining,
                                 MSG_NOSIGNAL);

        if (n > 0) {
            connection.written += static_cast<std::size_t>(n);
            metrics_.bump(metrics_.bytes_written, static_cast<std::uint64_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The kernel's send buffer is full: the client is slower than we
            // are. Keep the rest queued and ask to be told when it drains.
            break;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        close_connection(loop, fd);
        return;
    }

    if (connection.written == connection.outbox.size()) {
        connection.outbox.clear();
        connection.written = 0;
        if (connection.close_after_write) {
            close_connection(loop, fd);
            return;
        }
    }

    update_interest(loop, connection);
}

void Server::update_interest(Loop& loop, Connection& connection) {
    // Only ask for EPOLLOUT while there is something queued. Registering
    // permanent write interest would make epoll_wait return constantly for
    // every idle connection and spin the loop at 100% CPU.
    const bool pending = connection.written < connection.outbox.size();

    epoll_event event{};
    const std::uint32_t writable = pending ? static_cast<std::uint32_t>(EPOLLOUT) : 0u;
    event.events = static_cast<std::uint32_t>(EPOLLIN) | writable;
    event.data.fd = connection.fd.get();
    ::epoll_ctl(loop.epoll.get(), EPOLL_CTL_MOD, connection.fd.get(), &event);
}

void Server::handle_writable(Loop& loop, Connection& connection) {
    connection.last_active = std::chrono::steady_clock::now();
    flush(loop, connection);
}

void Server::close_connection(Loop& loop, int fd) {
    auto it = loop.connections.find(fd);
    if (it == loop.connections.end()) {
        return;
    }
    // EPOLL_CTL_DEL before close. A descriptor is removed from its epoll set
    // automatically on close, but only once every reference is gone -- being
    // explicit avoids a stale event for a number that may be reused.
    ::epoll_ctl(loop.epoll.get(), EPOLL_CTL_DEL, fd, nullptr);
    loop.connections.erase(it);  // destructor closes the descriptor
    metrics_.connections_current.fetch_sub(1, std::memory_order_relaxed);
}

void Server::sweep_idle(Loop& loop) {
    if (config_.idle_timeout.count() <= 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expired;

    for (const auto& [fd, connection] : loop.connections) {
        if (now - connection->last_active > config_.idle_timeout) {
            expired.push_back(fd);
        }
    }
    // Collected first, then closed: erasing while iterating the map would
    // invalidate the iterator.
    for (const int fd : expired) {
        close_connection(loop, fd);
    }
}

}  // namespace swiftkv
