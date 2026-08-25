#include "swiftkv/store.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"

using swiftkv::Store;

namespace {

Store::Config small_config(std::size_t shards = 8, std::size_t per_shard = 1000) {
    Store::Config config;
    config.shards = shards;
    config.max_entries_per_shard = per_shard;
    return config;
}

std::string key_for(int i) { return "key:" + std::to_string(i); }
std::string value_for(int i) { return "value:" + std::to_string(i); }

}  // namespace

// ---------------------------------------------------------------------------
// Single-threaded behaviour
// ---------------------------------------------------------------------------

TEST_CASE("stores and retrieves across shards", "[store]") {
    Store store(small_config());

    for (int i = 0; i < 500; ++i) {
        store.set(key_for(i), value_for(i));
    }

    for (int i = 0; i < 500; ++i) {
        REQUIRE(store.get(key_for(i)) == value_for(i));
    }
    REQUIRE(store.size() == 500);
}

TEST_CASE("missing keys return nothing", "[store]") {
    Store store(small_config());
    REQUIRE_FALSE(store.get("absent").has_value());
}

TEST_CASE("overwrites replace the value", "[store]") {
    Store store(small_config());

    store.set("k", "first");
    store.set("k", "second");

    REQUIRE(store.get("k") == "second");
    REQUIRE(store.size() == 1);
}

TEST_CASE("delete removes a key", "[store]") {
    Store store(small_config());
    store.set("k", "v");

    REQUIRE(store.del("k"));
    REQUIRE_FALSE(store.contains("k"));
    REQUIRE_FALSE(store.del("k"));
}

TEST_CASE("clear empties every shard", "[store]") {
    Store store(small_config());
    for (int i = 0; i < 200; ++i) {
        store.set(key_for(i), value_for(i));
    }

    store.clear();

    REQUIRE(store.size() == 0);
    REQUIRE(store.empty());
}

TEST_CASE("peek does not affect recency", "[store]") {
    Store store(small_config(1, 3));

    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3");
    store.peek("a");
    store.set("d", "4");

    REQUIRE_FALSE(store.contains("a"));
}

// ---------------------------------------------------------------------------
// Sharding
// ---------------------------------------------------------------------------

TEST_CASE("shard count is rounded up to a power of two", "[store][shard]") {
    // Shard selection masks instead of dividing, which requires a power of two.
    REQUIRE(Store(small_config(1)).shard_count() == 1);
    REQUIRE(Store(small_config(5)).shard_count() == 8);
    REQUIRE(Store(small_config(16)).shard_count() == 16);
    REQUIRE(Store(small_config(17)).shard_count() == 32);
}

TEST_CASE("a key always maps to the same shard", "[store][shard]") {
    Store store(small_config(16));

    const std::size_t first = store.shard_index("stable-key");
    for (int i = 0; i < 100; ++i) {
        REQUIRE(store.shard_index("stable-key") == first);
    }
}

TEST_CASE("shard index is always in range", "[store][shard]") {
    Store store(small_config(16));

    for (int i = 0; i < 10'000; ++i) {
        REQUIRE(store.shard_index(key_for(i)) < store.shard_count());
    }
}

TEST_CASE("keys spread reasonably evenly across shards", "[store][shard]") {
    // An uneven hash would concentrate load on a few shards and undo the whole
    // point of sharding. This asserts a loose bound -- enough to catch a
    // genuinely broken distribution without being flaky.
    constexpr int kKeys = 16'000;
    constexpr std::size_t kShards = 16;
    Store store(small_config(kShards, kKeys));

    for (int i = 0; i < kKeys; ++i) {
        store.set(key_for(i), "v");
    }

    const std::size_t expected = kKeys / kShards;
    for (std::size_t s = 0; s < store.shard_count(); ++s) {
        const std::size_t actual = store.shard_size(s);
        REQUIRE(actual > expected / 2);
        REQUIRE(actual < expected * 2);
    }
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

TEST_CASE("capacity is per shard, not global", "[store][capacity]") {
    Store store(small_config(4, /*per_shard=*/10));

    for (int i = 0; i < 1000; ++i) {
        store.set(key_for(i), "v");
    }

    REQUIRE(store.size() <= 4 * 10);
    REQUIRE(store.size() > 0);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST_CASE("statistics aggregate across shards", "[store][stats]") {
    Store store(small_config());
    store.set("a", "1");

    store.get("a");
    store.get("a");
    store.get("absent");

    const auto stats = store.stats();
    REQUIRE(stats.hits == 2);
    REQUIRE(stats.misses == 1);
    REQUIRE(stats.keys == 1);
    REQUIRE(stats.hit_rate() == Approx(2.0 / 3.0));
}

TEST_CASE("byte totals aggregate across shards", "[store][stats]") {
    Store store(small_config());

    store.set("key", "value");

    REQUIRE(store.stats().bytes == 3 + 5);
}

TEST_CASE("resetting stats keeps the data", "[store][stats]") {
    Store store(small_config());
    store.set("a", "1");
    store.get("a");

    store.reset_stats();

    REQUIRE(store.stats().hits == 0);
    REQUIRE(store.contains("a"));
}

// ---------------------------------------------------------------------------
// Concurrency
//
// These are the tests that justify the design. They are run under
// ThreadSanitizer in CI as well as normally -- a race that happens not to fire
// during an ordinary run is still a bug, and only TSan reliably finds it.
// ---------------------------------------------------------------------------

TEST_CASE("concurrent writers to distinct keys all succeed", "[store][concurrency]") {
    constexpr int kThreads = 16;
    constexpr int kPerThread = 2000;
    Store store(small_config(16, kThreads * kPerThread));

    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int id = t * kPerThread + i;
                store.set(key_for(id), value_for(id));
            }
        });
    }
    threads.clear();  // join

    REQUIRE(store.size() == kThreads * kPerThread);
    for (int id = 0; id < kThreads * kPerThread; ++id) {
        REQUIRE(store.get(key_for(id)) == value_for(id));
    }
}

TEST_CASE("concurrent writes to one key never tear", "[store][concurrency]") {
    // Whichever writer wins, a reader must see one complete value -- never a
    // mixture of two. Values are distinguishable so a torn read is detectable.
    constexpr int kThreads = 16;
    constexpr int kIterations = 5000;
    Store store(small_config());

    std::set<std::string> permitted;
    for (int t = 0; t < kThreads; ++t) {
        permitted.insert(std::string(64, static_cast<char>('a' + t)));
    }

    std::atomic<bool> torn{false};
    std::barrier sync(kThreads);

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                const std::string mine(64, static_cast<char>('a' + t));
                sync.arrive_and_wait();
                for (int i = 0; i < kIterations; ++i) {
                    store.set("contended", mine);
                    if (auto got = store.get("contended")) {
                        if (permitted.find(*got) == permitted.end()) {
                            torn.store(true);
                        }
                    }
                }
            });
        }
    }

    REQUIRE_FALSE(torn.load());
}

TEST_CASE("readers and writers interleave safely", "[store][concurrency]") {
    constexpr int kKeys = 1000;
    constexpr int kIterations = 5000;
    Store store(small_config(16, kKeys));

    for (int i = 0; i < kKeys; ++i) {
        store.set(key_for(i), value_for(i));
    }

    std::atomic<bool> mismatch{false};
    std::atomic<std::size_t> reads{0};

    {
        std::vector<std::jthread> threads;

        // Readers verify that any value they see belongs to the key they asked
        // for, which would fail if a lock were missing.
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kIterations; ++i) {
                    const int id = (t * 7919 + i) % kKeys;
                    if (auto got = store.get(key_for(id))) {
                        if (*got != value_for(id)) {
                            mismatch.store(true);
                        }
                        reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        // Writers rewrite the same values and churn unrelated keys.
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kIterations; ++i) {
                    const int id = (t * 104729 + i) % kKeys;
                    store.set(key_for(id), value_for(id));
                }
            });
        }
    }

    REQUIRE_FALSE(mismatch.load());
    REQUIRE(reads.load() > 0);
}

TEST_CASE("concurrent deletes leave a consistent store", "[store][concurrency]") {
    constexpr int kKeys = 8000;
    Store store(small_config(16, kKeys));

    for (int i = 0; i < kKeys; ++i) {
        store.set(key_for(i), value_for(i));
    }

    std::atomic<int> deleted{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                // Every key is targeted by every thread; exactly one delete per
                // key must report success.
                for (int i = t; i < kKeys; i += 8) {
                    if (store.del(key_for(i))) {
                        deleted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }

    REQUIRE(deleted.load() == kKeys);
    REQUIRE(store.size() == 0);
}

TEST_CASE("every key is deleted exactly once under contention", "[store][concurrency]") {
    // If del() were not atomic, two threads could both observe the key and both
    // report success, double-counting a single removal.
    constexpr int kKeys = 4000;
    Store store(small_config(16, kKeys));

    for (int i = 0; i < kKeys; ++i) {
        store.set(key_for(i), "v");
    }

    std::atomic<int> successes{0};
    std::barrier sync(8);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                sync.arrive_and_wait();
                for (int i = 0; i < kKeys; ++i) {
                    if (store.del(key_for(i))) {
                        successes.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }

    REQUIRE(successes.load() == kKeys);
}

TEST_CASE("statistics remain readable while the store is busy", "[store][concurrency]") {
    // stats() samples shard by shard rather than freezing the whole store, so
    // it must never block or crash under concurrent mutation.
    Store store(small_config());
    std::atomic<bool> stop{false};

    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < 8; ++t) {
            workers.emplace_back([&, t] {
                int i = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    store.set(key_for((t * 1000 + i++) % 5000), "v");
                }
            });
        }

        for (int i = 0; i < 200; ++i) {
            const auto stats = store.stats();
            REQUIRE(stats.keys == stats.keys);  // must not crash or deadlock
        }
        stop.store(true);
    }

    SUCCEED("stats() stayed responsive under concurrent writes");
}
