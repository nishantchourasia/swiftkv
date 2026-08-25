#include "swiftkv/latency.hpp"

#include <bit>
#include <limits>

namespace swiftkv {

LatencyHistogram::LatencyHistogram() noexcept { reset(); }

std::size_t LatencyHistogram::bucket_for(std::uint64_t value) noexcept {
    // Small values get their own bucket each: below kSubCount there is nothing
    // to interpolate, and exact counts are cheaper than arithmetic.
    if (value < kSubCount) {
        return static_cast<std::size_t>(value);
    }

    // exponent is the index of the highest set bit, so value lies in
    // [2^exponent, 2^(exponent+1)).
    const auto exponent = static_cast<std::size_t>(63 - std::countl_zero(value));

    // Take the kSubBits bits just below the leading one to pick the sub-bucket.
    // Subtracting kSubCount drops the implicit leading bit, leaving 0..15.
    const std::size_t sub =
        static_cast<std::size_t>(value >> (exponent - kSubBits)) - kSubCount;

    return (exponent - kSubBits + 1) * kSubCount + sub;
}

std::uint64_t LatencyHistogram::bucket_upper_bound(std::size_t bucket) noexcept {
    if (bucket < kSubCount) {
        return bucket;
    }

    const std::size_t octave = bucket / kSubCount;   // = exponent - kSubBits + 1
    const std::size_t sub = bucket % kSubCount;
    const std::size_t shift = octave - 1;            // = exponent - kSubBits

    // Lower bound of this bucket, then the last value that still maps here.
    const std::uint64_t lower = static_cast<std::uint64_t>(kSubCount + sub) << shift;
    const std::uint64_t width = std::uint64_t{1} << shift;
    return lower + width - 1;
}

void LatencyHistogram::record(std::uint64_t nanos) noexcept {
    const std::size_t bucket = bucket_for(nanos);
    if (bucket >= kBucketCount) {
        return;  // unreachable for any real duration; never index out of range
    }

    buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
    total_count_.fetch_add(1, std::memory_order_relaxed);
    total_sum_.fetch_add(nanos, std::memory_order_relaxed);

    // Compare-and-swap loops keep the true min and max, which the bucketed
    // values cannot give exactly.
    std::uint64_t prev_min = observed_min_.load(std::memory_order_relaxed);
    while (nanos < prev_min &&
           !observed_min_.compare_exchange_weak(prev_min, nanos, std::memory_order_relaxed)) {
    }

    std::uint64_t prev_max = observed_max_.load(std::memory_order_relaxed);
    while (nanos > prev_max &&
           !observed_max_.compare_exchange_weak(prev_max, nanos, std::memory_order_relaxed)) {
    }
}

LatencyHistogram::Snapshot LatencyHistogram::snapshot() const noexcept {
    Snapshot out;

    // Copy the buckets first, then derive every percentile from that copy. If
    // percentiles were computed by re-reading the live buckets, a concurrent
    // writer could make p95 come out below p50.
    std::array<std::uint64_t, kBucketCount> counts{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        counts[i] = buckets_[i].load(std::memory_order_relaxed);
        total += counts[i];
    }

    out.count = total;
    if (total == 0) {
        return out;
    }

    const std::uint64_t sum = total_sum_.load(std::memory_order_relaxed);
    const std::uint64_t count_for_mean = total_count_.load(std::memory_order_relaxed);
    out.mean_ns = count_for_mean == 0
                      ? 0.0
                      : static_cast<double>(sum) / static_cast<double>(count_for_mean);

    const std::uint64_t seen_min = observed_min_.load(std::memory_order_relaxed);
    out.min_ns = seen_min == std::numeric_limits<std::uint64_t>::max() ? 0 : seen_min;
    out.max_ns = observed_max_.load(std::memory_order_relaxed);

    // Walk the buckets once, emitting each percentile as its threshold is
    // crossed. Percentiles are requested in ascending order so one pass covers
    // all of them.
    struct Target {
        double fraction;
        std::uint64_t* out;
    };
    const Target targets[] = {
        {0.50, &out.p50_ns}, {0.90, &out.p90_ns}, {0.95, &out.p95_ns},
        {0.99, &out.p99_ns}, {0.999, &out.p999_ns},
    };
    constexpr std::size_t kTargets = sizeof(targets) / sizeof(targets[0]);

    std::size_t next = 0;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kBucketCount && next < kTargets; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        cumulative += counts[i];
        const double reached = static_cast<double>(cumulative) / static_cast<double>(total);
        while (next < kTargets && reached >= targets[next].fraction) {
            *targets[next].out = bucket_upper_bound(i);
            ++next;
        }
    }

    // If rounding left any target unfilled, the largest observed bucket is the
    // correct answer for it.
    for (; next < kTargets; ++next) {
        *targets[next].out = out.max_ns;
    }

    return out;
}

void LatencyHistogram::reset() noexcept {
    for (auto& bucket : buckets_) {
        bucket.store(0, std::memory_order_relaxed);
    }
    total_count_.store(0, std::memory_order_relaxed);
    total_sum_.store(0, std::memory_order_relaxed);
    observed_min_.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
    observed_max_.store(0, std::memory_order_relaxed);
}

}  // namespace swiftkv
