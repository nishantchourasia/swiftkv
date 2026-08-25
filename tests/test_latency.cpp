#include "swiftkv/latency.hpp"

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "catch.hpp"

using swiftkv::LatencyHistogram;

namespace {

/// Exact percentile over a sorted copy, to check the histogram against.
std::uint64_t exact_percentile(std::vector<std::uint64_t> values, double p) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[std::min(index, values.size() - 1)];
}

}  // namespace

// ---------------------------------------------------------------------------
// Bucket arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("small values get exact buckets", "[latency]") {
    for (std::uint64_t v = 0; v < LatencyHistogram::kSubCount; ++v) {
        REQUIRE(LatencyHistogram::bucket_for(v) == v);
        REQUIRE(LatencyHistogram::bucket_upper_bound(v) == v);
    }
}

TEST_CASE("bucket indices stay within the array", "[latency]") {
    // Out-of-range indexing here would be a memory-safety bug, not a metrics bug.
    for (int shift = 0; shift < 64; ++shift) {
        const std::uint64_t v = std::uint64_t{1} << shift;
        REQUIRE(LatencyHistogram::bucket_for(v) < LatencyHistogram::kBucketCount);
        if (v > 1) {
            REQUIRE(LatencyHistogram::bucket_for(v - 1) < LatencyHistogram::kBucketCount);
        }
    }
    REQUIRE(LatencyHistogram::bucket_for(UINT64_MAX) < LatencyHistogram::kBucketCount);
}

TEST_CASE("buckets increase monotonically with value", "[latency]") {
    std::size_t previous = 0;
    for (std::uint64_t v = 1; v < 10'000'000; v = v + 1 + v / 8) {
        const std::size_t bucket = LatencyHistogram::bucket_for(v);
        REQUIRE(bucket >= previous);
        previous = bucket;
    }
}

TEST_CASE("a value never exceeds its bucket's upper bound", "[latency]") {
    // This is what guarantees percentiles overstate rather than understate.
    for (std::uint64_t v = 1; v < 100'000'000; v = v + 1 + v / 7) {
        const std::size_t bucket = LatencyHistogram::bucket_for(v);
        REQUIRE(v <= LatencyHistogram::bucket_upper_bound(bucket));
    }
}

TEST_CASE("bucket error stays within the documented 6.25%", "[latency]") {
    // The README and the class docs both claim at most 1/16 overstatement.
    for (std::uint64_t v = 1; v < 100'000'000; v = v + 1 + v / 11) {
        const std::size_t bucket = LatencyHistogram::bucket_for(v);
        const std::uint64_t upper = LatencyHistogram::bucket_upper_bound(bucket);
        const double error = static_cast<double>(upper - v) / static_cast<double>(v);
        INFO("value " << v << " -> upper bound " << upper);
        REQUIRE(error <= 0.0625);
    }
}

// ---------------------------------------------------------------------------
// Recording and summary
// ---------------------------------------------------------------------------

TEST_CASE("an empty histogram reports nothing", "[latency]") {
    LatencyHistogram histogram;
    const auto snapshot = histogram.snapshot();

    REQUIRE(snapshot.empty());
    REQUIRE(snapshot.count == 0);
    REQUIRE(snapshot.p99_ns == 0);
}

TEST_CASE("counts every observation", "[latency]") {
    LatencyHistogram histogram;
    for (int i = 0; i < 1000; ++i) {
        histogram.record(1000);
    }

    REQUIRE(histogram.snapshot().count == 1000);
}

TEST_CASE("min and max are exact, not bucketed", "[latency]") {
    LatencyHistogram histogram;
    histogram.record(1234);
    histogram.record(9'876'543);
    histogram.record(50'000);

    const auto snapshot = histogram.snapshot();
    REQUIRE(snapshot.min_ns == 1234);
    REQUIRE(snapshot.max_ns == 9'876'543);
}

TEST_CASE("mean is computed from exact values", "[latency]") {
    LatencyHistogram histogram;
    histogram.record(100);
    histogram.record(200);
    histogram.record(300);

    REQUIRE(histogram.snapshot().mean_ns == Approx(200.0));
}

TEST_CASE("a uniform distribution yields the expected percentiles", "[latency]") {
    LatencyHistogram histogram;
    std::vector<std::uint64_t> values;
    for (std::uint64_t v = 1; v <= 10'000; ++v) {
        histogram.record(v * 1000);  // 1us .. 10ms
        values.push_back(v * 1000);
    }

    const auto snapshot = histogram.snapshot();

    // Each reported percentile must be at least the true one (never understate)
    // and within the documented resolution above it.
    for (auto [reported, p] : {std::pair{snapshot.p50_ns, 0.50},
                               std::pair{snapshot.p95_ns, 0.95},
                               std::pair{snapshot.p99_ns, 0.99}}) {
        const std::uint64_t exact = exact_percentile(values, p);
        INFO("p" << p * 100 << ": reported " << reported << " exact " << exact);
        REQUIRE(reported >= exact);
        REQUIRE(static_cast<double>(reported) <= static_cast<double>(exact) * 1.07);
    }
}

TEST_CASE("percentiles are ordered", "[latency]") {
    LatencyHistogram histogram;
    std::mt19937_64 rng(42);
    std::lognormal_distribution<double> dist(10.0, 1.5);
    for (int i = 0; i < 50'000; ++i) {
        histogram.record(static_cast<std::uint64_t>(dist(rng)) + 1);
    }

    const auto s = histogram.snapshot();
    REQUIRE(s.min_ns <= s.p50_ns);
    REQUIRE(s.p50_ns <= s.p90_ns);
    REQUIRE(s.p90_ns <= s.p95_ns);
    REQUIRE(s.p95_ns <= s.p99_ns);
    REQUIRE(s.p99_ns <= s.p999_ns);
    REQUIRE(s.p999_ns <= s.max_ns);
}

TEST_CASE("a heavy tail is visible in p99 but not in the mean", "[latency]") {
    // The reason the histogram exists: an average hides the failure.
    LatencyHistogram histogram;
    for (int i = 0; i < 9900; ++i) {
        histogram.record(1'000'000);  // 1ms
    }
    for (int i = 0; i < 100; ++i) {
        histogram.record(2'000'000'000);  // 2s
    }

    const auto s = histogram.snapshot();

    REQUIRE(s.p50_ns < 1'100'000);            // typical request unaffected
    REQUIRE(s.p999_ns > 1'000'000'000);       // the tail is plainly visible
    REQUIRE(s.mean_ns < 25'000'000);          // the mean does not reveal it
}

TEST_CASE("reset clears everything", "[latency]") {
    LatencyHistogram histogram;
    histogram.record(1000);
    histogram.reset();

    const auto snapshot = histogram.snapshot();
    REQUIRE(snapshot.count == 0);
    REQUIRE(snapshot.min_ns == 0);
    REQUIRE(snapshot.max_ns == 0);
}

TEST_CASE("zero is recordable", "[latency]") {
    // A clock with coarse resolution can legitimately return a zero duration.
    LatencyHistogram histogram;
    histogram.record(0);

    REQUIRE(histogram.snapshot().count == 1);
}

// ---------------------------------------------------------------------------
// Concurrency -- every event loop records into one histogram
// ---------------------------------------------------------------------------

TEST_CASE("concurrent recording loses no observations", "[latency][concurrency]") {
    constexpr int kThreads = 16;
    constexpr int kPerThread = 20'000;
    LatencyHistogram histogram;

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&histogram, t] {
                std::mt19937_64 rng(static_cast<std::uint64_t>(t));
                std::uniform_int_distribution<std::uint64_t> dist(1, 1'000'000);
                for (int i = 0; i < kPerThread; ++i) {
                    histogram.record(dist(rng));
                }
            });
        }
    }

    REQUIRE(histogram.snapshot().count == kThreads * kPerThread);
}

TEST_CASE("snapshots stay coherent while writers run", "[latency][concurrency]") {
    // A snapshot taken mid-write must still be internally consistent: reading
    // the live buckets per-percentile could otherwise report p95 below p50.
    LatencyHistogram histogram;
    std::atomic<bool> stop{false};

    {
        std::vector<std::jthread> writers;
        for (int t = 0; t < 8; ++t) {
            writers.emplace_back([&histogram, &stop] {
                std::mt19937_64 rng(std::random_device{}());
                std::uniform_int_distribution<std::uint64_t> dist(1, 10'000'000);
                while (!stop.load(std::memory_order_relaxed)) {
                    histogram.record(dist(rng));
                }
            });
        }

        for (int i = 0; i < 500; ++i) {
            const auto s = histogram.snapshot();
            REQUIRE(s.p50_ns <= s.p95_ns);
            REQUIRE(s.p95_ns <= s.p99_ns);
        }
        stop.store(true);
    }

    SUCCEED("snapshots remained ordered under concurrent recording");
}
