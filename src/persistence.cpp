#include "swiftkv/persistence.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace swiftkv {
namespace {

constexpr std::size_t kReplayChunk = 1 << 20;  // 1 MiB

}  // namespace

std::string to_string(AppendOnlyLog::SyncPolicy policy) {
    switch (policy) {
        case AppendOnlyLog::SyncPolicy::Always:
            return "always";
        case AppendOnlyLog::SyncPolicy::EverySecond:
            return "everysec";
        case AppendOnlyLog::SyncPolicy::Never:
            return "never";
    }
    return "everysec";
}

bool parse_sync_policy(const std::string& text, AppendOnlyLog::SyncPolicy& out) {
    if (text == "always") {
        out = AppendOnlyLog::SyncPolicy::Always;
        return true;
    }
    if (text == "everysec") {
        out = AppendOnlyLog::SyncPolicy::EverySecond;
        return true;
    }
    if (text == "never") {
        out = AppendOnlyLog::SyncPolicy::Never;
        return true;
    }
    return false;
}

AppendOnlyLog::AppendOnlyLog(Config config) : config_(std::move(config)) {}

AppendOnlyLog::~AppendOnlyLog() { close(); }

NetResult AppendOnlyLog::open() {
    if (config_.path.empty()) {
        NetResult result;
        result.ok = false;
        result.error = EINVAL;
        result.message = "append-only log path is empty";
        return result;
    }

    // O_APPEND makes every write land at the end of the file atomically with
    // respect to other writers, so a concurrent rewrite or an external process
    // cannot interleave into the middle of a record.
    fd_.reset(::open(config_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
    if (!fd_) {
        return NetResult::failure("open(append-only log)");
    }

    if (config_.sync == SyncPolicy::EverySecond) {
        wake_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (!wake_fd_) {
            fd_.reset();
            return NetResult::failure("eventfd(append-only log)");
        }
        // No synchronisation needed for this store: the flusher does not exist
        // yet, and constructing a std::thread synchronises everything written
        // before it with the new thread.
        stopping_.store(false, std::memory_order_relaxed);
        flusher_ = std::thread([this] { run_flusher(); });
    }

    return NetResult::success();
}

void AppendOnlyLog::close() {
    if (flusher_.joinable()) {
        // Set the flag before signalling. The flusher re-checks it after poll()
        // returns, so there is no lost-wakeup window: either it sees the flag on
        // its next check, or poll() reports the eventfd and it checks again.
        stopping_.store(true, std::memory_order_release);
        const std::uint64_t one = 1;
        ssize_t ignored = ::write(wake_fd_.get(), &one, sizeof(one));
        (void)ignored;
        flusher_.join();
    }
    wake_fd_.reset();

    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_) {
        flush_locked();
        ::fsync(fd_.get());
    }
    // Reset inside the lock. Previously this sat outside it, so closing the
    // descriptor raced with append() and sync() reading fd_ under the lock --
    // and worse than a race, an append could pass its `if (!fd_)` check, have
    // the descriptor closed underneath it, and then write to a number the
    // kernel had already handed to some other file. ThreadSanitizer caught it.
    fd_.reset();
}

void AppendOnlyLog::append(const std::vector<std::string>& command) {
    const std::string record = encode_array(command);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!fd_) {
        return;
    }

    pending_ += record;
    ++stats_.records_appended;

    if (config_.sync == SyncPolicy::Always) {
        // Durable before the caller continues. This is the expensive policy:
        // every write waits for the device.
        flush_locked();
        ::fsync(fd_.get());
        ++stats_.fsyncs;
    } else if (pending_.size() >= (1 << 16)) {
        // Bound the buffer even between scheduled flushes, so a burst of writes
        // cannot grow it without limit.
        flush_locked();
    }
}

void AppendOnlyLog::flush_locked() {
    if (pending_.empty() || !fd_) {
        return;
    }

    std::size_t written = 0;
    while (written < pending_.size()) {
        const ssize_t n =
            ::write(fd_.get(), pending_.data() + written, pending_.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;  // give up on this flush; the data stays buffered
    }

    stats_.bytes_appended += written;
    pending_.erase(0, written);
}

void AppendOnlyLog::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fd_) {
        return;
    }
    flush_locked();
    ::fsync(fd_.get());
    ++stats_.fsyncs;
}

void AppendOnlyLog::run_flusher() {
    const int timeout_ms = static_cast<int>(config_.flush_interval.count());

    while (!stopping_.load(std::memory_order_acquire)) {
        // Block holding no lock at all. poll() returns either when the interval
        // elapses or as soon as close() writes to the eventfd, so shutdown is
        // immediate rather than waiting out a full interval.
        pollfd waiter{};
        waiter.fd = wake_fd_.get();
        waiter.events = POLLIN;

        const int ready = ::poll(&waiter, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (waiter.revents & POLLIN) {
            std::uint64_t drained = 0;
            ssize_t ignored = ::read(wake_fd_.get(), &drained, sizeof(drained));
            (void)ignored;
        }
        if (stopping_.load(std::memory_order_acquire)) {
            break;  // close() performs the final flush and fsync
        }

        // Take the data lock only for the flush itself.
        sync();
    }
}

AppendOnlyLog::ReplayResult AppendOnlyLog::replay(Store& store) {
    ReplayResult result;

    std::ifstream input(config_.path, std::ios::binary);
    if (!input) {
        // A missing log is the normal first start, not a failure.
        result.ok = true;
        result.message = "no existing log";
        return result;
    }

    std::string buffer;
    std::string chunk(kReplayChunk, '\0');

    // Replay limits must not reject a record the server itself accepted, so
    // they are relaxed relative to the wire defaults.
    Limits limits;
    limits.max_arg_bytes = 512u * 1024 * 1024;
    limits.max_request_bytes = 1024u * 1024 * 1024;

    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(input.gcount());
        if (got == 0) {
            break;
        }
        buffer.append(chunk.data(), got);
        result.bytes_read += got;

        while (true) {
            Command command;
            const ParseResult parsed = parse_command(buffer, command, limits);

            if (parsed.status == ParseStatus::Incomplete) {
                break;  // need more bytes from the file
            }
            if (parsed.fatal()) {
                // Genuinely malformed content in the middle of the file, as
                // opposed to a partial record at the end. Stop and say so
                // rather than guessing.
                result.ok = false;
                result.message = "corrupt record: " + parsed.message;
                result.bytes_discarded = buffer.size();
                return result;
            }

            buffer.erase(0, parsed.consumed);

            const std::string verb = command.verb();
            if (verb == "SET" && command.size() == 3) {
                store.set(command.args[1], command.args[2]);
                ++result.commands_applied;
            } else if (verb == "DEL" && command.size() >= 2) {
                for (std::size_t i = 1; i < command.size(); ++i) {
                    store.del(command.args[i]);
                }
                ++result.commands_applied;
            } else if (verb == "FLUSHALL") {
                store.clear();
                ++result.commands_applied;
            }
            // Anything else in the log is ignored: only mutations matter.
        }
    }

    // Whatever is left could not form a complete record. That is the expected
    // result of a crash mid-write, and the command was never acknowledged.
    result.bytes_discarded = buffer.size();
    result.ok = true;
    if (result.bytes_discarded > 0) {
        result.message = "discarded a partial trailing record";
    }
    return result;
}

NetResult AppendOnlyLog::rewrite(Store& store) {
    const std::string temp_path = config_.path + ".rewrite";

    {
        FileDescriptor temp(
            ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (!temp) {
            return NetResult::failure("open(rewrite temp)");
        }

        // One SET per surviving key: the shortest command sequence that
        // reproduces the current state.
        std::string batch;
        auto flush_batch = [&]() -> bool {
            std::size_t written = 0;
            while (written < batch.size()) {
                const ssize_t n =
                    ::write(temp.get(), batch.data() + written, batch.size() - written);
                if (n > 0) {
                    written += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }
            batch.clear();
            return true;
        };

        bool ok = true;
        store.for_each([&](const std::string& key, const std::string& value) {
            if (!ok) {
                return;
            }
            batch += encode_array({"SET", key, value});
            if (batch.size() >= (1 << 20)) {
                ok = flush_batch();
            }
        });

        if (!ok || !flush_batch()) {
            ::unlink(temp_path.c_str());
            return NetResult::failure("write(rewrite temp)");
        }

        // fsync the replacement before it is put in place. Renaming a file
        // whose contents are still only in the page cache would, after a power
        // cut, leave an empty file where the log used to be.
        if (::fsync(temp.get()) != 0) {
            ::unlink(temp_path.c_str());
            return NetResult::failure("fsync(rewrite temp)");
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // rename is atomic: readers see either the old log or the new one, never a
    // half-written file.
    if (::rename(temp_path.c_str(), config_.path.c_str()) != 0) {
        ::unlink(temp_path.c_str());
        return NetResult::failure("rename(rewrite)");
    }

    // Reopen: the old descriptor still refers to the now-unlinked inode, so
    // further appends would vanish.
    pending_.clear();
    fd_.reset(::open(config_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
    if (!fd_) {
        return NetResult::failure("reopen after rewrite");
    }

    ++stats_.rewrites;
    return NetResult::success();
}

AppendOnlyLog::Stats AppendOnlyLog::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::uint64_t AppendOnlyLog::size_on_disk() const {
    std::ifstream input(config_.path, std::ios::binary | std::ios::ate);
    if (!input) {
        return 0;
    }
    return static_cast<std::uint64_t>(input.tellg());
}

}  // namespace swiftkv
