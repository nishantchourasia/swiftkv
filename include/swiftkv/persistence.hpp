#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "swiftkv/net.hpp"
#include "swiftkv/protocol.hpp"
#include "swiftkv/store.hpp"

namespace swiftkv {

/// Append-only log: the store's durability mechanism.
///
/// Every write is appended to the end of a file before it is acknowledged (or
/// shortly after -- see `SyncPolicy`). On restart the log is replayed from the
/// beginning to rebuild the store. This is how Redis's AOF works, and it is
/// chosen over periodic snapshots for two reasons:
///
///   * **Appending is sequential**, which is the one access pattern every
///     storage device is fast at. A snapshot must serialise the whole dataset
///     and stalls writes while it does so.
///   * **A crash costs at most the unsynced tail**, not the entire interval
///     since the last snapshot.
///
/// ### The log stores RESP, not a bespoke format
///
/// Records are the same encoded commands the wire protocol already uses, so
/// replay is the existing parser applied to a file instead of a socket. A
/// separate serialisation format would be a second thing to get right, a second
/// thing to version, and a second place for the two to disagree.
///
/// ### A truncated tail is expected, not corruption
///
/// If the process dies mid-write the file ends with a partial record. Replay
/// stops cleanly at that point and reports how many bytes were discarded. That
/// is the correct behaviour: the client never received an acknowledgement for
/// that command, so dropping it loses nothing that was ever promised.
class AppendOnlyLog {
public:
    /// When the log is flushed to the operating system and forced to disk.
    ///
    /// `fsync` is what actually makes data survive a power cut. Writing to a
    /// file only hands bytes to the kernel's page cache; without an fsync they
    /// can sit in memory for seconds.
    enum class SyncPolicy {
        /// fsync before acknowledging each write. Safest, and slowest by far --
        /// every SET waits for the disk.
        Always,
        /// fsync once a second in the background. A crash loses at most one
        /// second of writes. This is Redis's default and the sensible trade.
        EverySecond,
        /// Never fsync explicitly; let the kernel decide. Fastest, and a power
        /// cut can lose everything not yet written back.
        Never,
    };

    struct Config {
        std::string path;
        SyncPolicy sync = SyncPolicy::EverySecond;
        std::chrono::milliseconds flush_interval{1000};
    };

    struct ReplayResult {
        bool ok = false;
        std::uint64_t commands_applied = 0;
        std::uint64_t bytes_read = 0;
        std::uint64_t bytes_discarded = 0;  ///< partial record at the tail
        std::string message;
    };

    struct Stats {
        std::uint64_t records_appended = 0;
        std::uint64_t bytes_appended = 0;
        std::uint64_t fsyncs = 0;
        std::uint64_t rewrites = 0;
    };

    explicit AppendOnlyLog(Config config);
    ~AppendOnlyLog();

    AppendOnlyLog(const AppendOnlyLog&) = delete;
    AppendOnlyLog& operator=(const AppendOnlyLog&) = delete;

    /// Open the log for appending, creating it if absent.
    NetResult open();

    /// Stop the flusher, flush what is buffered, and close the file.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return fd_.valid(); }

    /// Record a write. Safe to call from any number of threads.
    void append(const std::vector<std::string>& command);

    /// Force everything buffered out to disk now.
    void sync();

    /// Rebuild `store` by replaying the log from the start.
    ///
    /// Call before serving: it does not lock, because nothing else should be
    /// touching the store yet.
    ReplayResult replay(Store& store);

    /// Rewrite the log as the shortest sequence of commands that reproduces the
    /// current contents of `store`.
    ///
    /// Needed because the log records *history*, not state: a key written a
    /// million times occupies a million records even though one would do. The
    /// rewrite goes to a temporary file which is then renamed over the original,
    /// so a crash midway leaves the previous log intact rather than a truncated
    /// one.
    NetResult rewrite(Store& store);

    [[nodiscard]] Stats stats() const;

    /// Bytes currently in the log file.
    [[nodiscard]] std::uint64_t size_on_disk() const;

private:
    void run_flusher();
    /// Write out the pending buffer. Caller must hold `mutex_`.
    void flush_locked();

    Config config_;
    FileDescriptor fd_;

    /// Guards the buffer, the descriptor and the counters. Held only for short
    /// critical sections, never across a wait.
    mutable std::mutex mutex_;
    std::string pending_;
    Stats stats_;

    std::thread flusher_;

    /// Wakeup channel for the flusher: an eventfd polled with a timeout, rather
    /// than a condition variable.
    ///
    /// A condition variable is the textbook choice and was tried first. It was
    /// replaced for two reasons, one about design and one about tooling.
    ///
    /// The design reason: a cv requires holding a mutex across the wait. That
    /// either entangles the data lock with the wait, or needs a second mutex
    /// purely for the wakeup. An eventfd needs neither -- the flusher blocks in
    /// poll() holding nothing at all, and takes the data lock only for the
    /// brief flush itself.
    ///
    /// The tooling reason: on this toolchain (gcc 11 + glibc 2.35),
    /// `condition_variable::wait_for` uses a steady clock and so routes through
    /// `pthread_cond_clockwait`, which ThreadSanitizer does not intercept. TSan
    /// therefore never observed the mutex being released during the wait,
    /// reported a spurious "double lock", and -- because its model of that
    /// mutex was then inconsistent -- lost the happens-before edges through it,
    /// cascading into a dozen false data-race reports. Verified by substituting
    /// a system_clock wait, which TSan does intercept: the reports went to zero
    /// with no change in the locking. Rather than adopt a wall clock that can
    /// jump backwards, or suppress a warning, the wait was removed entirely.
    FileDescriptor wake_fd_;
    std::atomic<bool> stopping_{false};
};

/// Human-readable name for a sync policy, and the reverse.
std::string to_string(AppendOnlyLog::SyncPolicy policy);
bool parse_sync_policy(const std::string& text, AppendOnlyLog::SyncPolicy& out);

}  // namespace swiftkv
