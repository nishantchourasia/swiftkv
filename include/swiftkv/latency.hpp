#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace swiftkv {

/// A latency histogram that many threads can write to without locking.
///
/// ### Why a histogram and not a running average
///
/// An average hides the failure. If 99 requests take 1 ms and one takes two
/// seconds, the mean is 21 ms and the disaster is invisible. What matters is
/// the tail -- p99 and p99.9 -- because that is the experience a real user
/// complains about. Percentiles cannot be derived from a mean, so the shape of
/// the distribution has to be kept.
///
/// ### Why not keep every sample
///
/// The benchmark client does keep every sample: it runs for a bounded time and
/// can afford the memory. The server cannot. It runs for weeks, so storage must
/// be constant, and it records on the hot path of every command, so recording
/// must not allocate, lock, or grow.
///
/// ### The bucket layout
///
/// Buckets are log-linear, the scheme HdrHistogram uses. Each doubling of
/// latency is divided into 16 linear sub-buckets, so resolution stays
/// proportional across the whole range: fine where latencies are small and
/// coarse where they are large, which is what percentile reporting wants.
///
/// The cost is that a reported value is the bucket's **upper** bound rather
/// than the exact sample. Worst-case overstatement is 1/16, i.e. **at most
/// 6.25%**, and the error is always in the direction of reporting a latency as
/// slightly worse than it was. Overstating is the safe direction for a
/// reliability metric; a histogram that flattered the tail would be worse than
/// none.
///
/// ### Thread safety
///
/// `record` is a single relaxed atomic increment on one bucket. Relaxed is
/// correct here because nothing branches on these counters -- they are
/// statistics, not synchronisation -- and the only requirement is that
/// increments are not lost.
///
/// `snapshot` reads the buckets without stopping writers, so it is a sample
/// rather than an instant: a very fast concurrent writer can land in a bucket
/// already counted. For a metric that is the right trade against freezing the
/// server to answer a dashboard poll.
class LatencyHistogram {
public:
    /// Sub-buckets per doubling. 16 gives at most 6.25% overstatement.
    static constexpr std::size_t kSubBits = 4;
    static constexpr std::size_t kSubCount = std::size_t{1} << kSubBits;

    /// Covers 1 ns to roughly 2^63 ns (~292 years), so nothing can overflow the
    /// range in practice.
    static constexpr std::size_t kBucketCount = (64 - kSubBits + 1) * kSubCount;

    struct Snapshot {
        std::uint64_t count = 0;
        std::uint64_t min_ns = 0;
        std::uint64_t max_ns = 0;
        double mean_ns = 0.0;
        std::uint64_t p50_ns = 0;
        std::uint64_t p90_ns = 0;
        std::uint64_t p95_ns = 0;
        std::uint64_t p99_ns = 0;
        std::uint64_t p999_ns = 0;

        [[nodiscard]] bool empty() const noexcept { return count == 0; }
    };

    LatencyHistogram() noexcept;

    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    /// Record one observation, in nanoseconds.
    void record(std::uint64_t nanos) noexcept;

    /// Read the current distribution.
    [[nodiscard]] Snapshot snapshot() const noexcept;

    /// Clear every bucket.
    void reset() noexcept;

    /// Which bucket a value falls in. Exposed for testing.
    [[nodiscard]] static std::size_t bucket_for(std::uint64_t value) noexcept;

    /// The largest value that bucket represents. Percentiles report this, so
    /// they never understate a latency.
    [[nodiscard]] static std::uint64_t bucket_upper_bound(std::size_t bucket) noexcept;

private:
    std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_;
    std::atomic<std::uint64_t> total_count_{0};
    std::atomic<std::uint64_t> total_sum_{0};
    std::atomic<std::uint64_t> observed_min_{UINT64_MAX};
    std::atomic<std::uint64_t> observed_max_{0};
};

}  // namespace swiftkv
