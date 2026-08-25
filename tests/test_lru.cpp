#include "swiftkv/lru.hpp"

#include <string>

#include "catch.hpp"

using swiftkv::LruCache;

namespace {

std::string big(std::size_t n, char fill = 'x') { return std::string(n, fill); }

}  // namespace

TEST_CASE("stores and retrieves values", "[lru]") {
    LruCache cache(4);

    cache.put("a", "1");
    cache.put("b", "2");

    REQUIRE(cache.get("a") == "1");
    REQUIRE(cache.get("b") == "2");
    REQUIRE(cache.size() == 2);
}

TEST_CASE("missing key returns nothing", "[lru]") {
    LruCache cache(4);

    REQUIRE_FALSE(cache.get("absent").has_value());
    REQUIRE(cache.stats().misses == 1);
    REQUIRE(cache.stats().hits == 0);
}

TEST_CASE("overwriting a key replaces its value without growing the cache", "[lru]") {
    LruCache cache(4);

    cache.put("k", "first");
    cache.put("k", "second");

    REQUIRE(cache.get("k") == "second");
    REQUIRE(cache.size() == 1);
}

TEST_CASE("overwriting adjusts the byte total in both directions", "[lru]") {
    LruCache cache(4);

    cache.put("k", big(100));
    const std::size_t after_large = cache.bytes();

    cache.put("k", big(10));
    const std::size_t after_small = cache.bytes();

    REQUIRE(after_large == 1 + 100);
    REQUIRE(after_small == 1 + 10);
}

TEST_CASE("erase removes a key", "[lru]") {
    LruCache cache(4);
    cache.put("a", "1");

    REQUIRE(cache.erase("a"));
    REQUIRE_FALSE(cache.contains("a"));
    REQUIRE(cache.bytes() == 0);
    REQUIRE_FALSE(cache.erase("a"));  // erasing twice is not an error
}

TEST_CASE("clear empties the cache", "[lru]") {
    LruCache cache(4);
    cache.put("a", "1");
    cache.put("b", "2");

    cache.clear();

    REQUIRE(cache.empty());
    REQUIRE(cache.bytes() == 0);
}

// ---------------------------------------------------------------------------
// Eviction order -- the part that makes it an LRU rather than a hash map
// ---------------------------------------------------------------------------

TEST_CASE("evicts the least recently used key when full", "[lru][eviction]") {
    LruCache cache(3);

    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    cache.put("d", "4");  // pushes out "a", the oldest

    REQUIRE(cache.size() == 3);
    REQUIRE_FALSE(cache.contains("a"));
    REQUIRE(cache.contains("d"));
    REQUIRE(cache.stats().evictions == 1);
}

TEST_CASE("reading a key protects it from the next eviction", "[lru][eviction]") {
    LruCache cache(3);

    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    cache.get("a");       // "a" becomes most recent, so "b" is now oldest
    cache.put("d", "4");

    REQUIRE(cache.contains("a"));
    REQUIRE_FALSE(cache.contains("b"));
}

TEST_CASE("peek does not protect a key from eviction", "[lru][eviction]") {
    // A background reader must not be able to keep a key alive that no client
    // is actually using.
    LruCache cache(3);

    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    REQUIRE(cache.peek("a") == "1");
    cache.put("d", "4");

    REQUIRE_FALSE(cache.contains("a"));
}

TEST_CASE("overwriting a key also refreshes its recency", "[lru][eviction]") {
    LruCache cache(3);

    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    cache.put("a", "updated");
    cache.put("d", "4");

    REQUIRE(cache.contains("a"));
    REQUIRE_FALSE(cache.contains("b"));
}

TEST_CASE("lru_key reports the next victim", "[lru][eviction]") {
    LruCache cache(3);

    cache.put("a", "1");
    cache.put("b", "2");

    REQUIRE(cache.lru_key() == "a");
    cache.get("a");
    REQUIRE(cache.lru_key() == "b");
}

TEST_CASE("an empty cache has no victim", "[lru][eviction]") {
    LruCache cache(3);
    REQUIRE_FALSE(cache.lru_key().has_value());
}

// ---------------------------------------------------------------------------
// Byte bound -- what actually protects the server's memory
// ---------------------------------------------------------------------------

TEST_CASE("evicts to stay within the byte budget", "[lru][bytes]") {
    // Entry count is generous; bytes are the binding constraint.
    LruCache cache(100, /*max_bytes=*/50);

    cache.put("a", big(20));
    cache.put("b", big(20));
    REQUIRE(cache.size() == 2);

    cache.put("c", big(20));  // 63 bytes total would exceed 50

    REQUIRE(cache.bytes() <= 50);
    REQUIRE_FALSE(cache.contains("a"));
}

TEST_CASE("a value larger than the whole budget is still stored", "[lru][bytes]") {
    // Otherwise every write of an oversized value would be silently discarded
    // and the cache would sit permanently empty.
    LruCache cache(10, /*max_bytes=*/50);

    cache.put("k", big(500));

    REQUIRE(cache.contains("k"));
    REQUIRE(cache.get("k")->size() == 500);
}

TEST_CASE("byte accounting counts keys as well as values", "[lru][bytes]") {
    LruCache cache(10);

    cache.put("key", "value");

    REQUIRE(cache.bytes() == 3 + 5);
}

TEST_CASE("zero byte bound means unbounded bytes", "[lru][bytes]") {
    LruCache cache(10, /*max_bytes=*/0);

    cache.put("k", big(10'000));

    REQUIRE(cache.contains("k"));
}

TEST_CASE("a zero-entry cache is treated as holding one", "[lru][bytes]") {
    // Almost certainly a misconfiguration; degrade rather than discard writes.
    LruCache cache(0);

    cache.put("a", "1");

    REQUIRE(cache.size() == 1);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST_CASE("counts hits and misses", "[lru][stats]") {
    LruCache cache(4);
    cache.put("a", "1");

    cache.get("a");
    cache.get("a");
    cache.get("absent");

    REQUIRE(cache.stats().hits == 2);
    REQUIRE(cache.stats().misses == 1);
    REQUIRE(cache.hit_rate() == Approx(2.0 / 3.0));
}

TEST_CASE("hit rate is zero before any lookup", "[lru][stats]") {
    LruCache cache(4);
    REQUIRE(cache.hit_rate() == Approx(0.0));
}

TEST_CASE("peek does not count as a hit", "[lru][stats]") {
    LruCache cache(4);
    cache.put("a", "1");

    cache.peek("a");

    REQUIRE(cache.stats().hits == 0);
}

TEST_CASE("stats can be reset without clearing data", "[lru][stats]") {
    LruCache cache(4);
    cache.put("a", "1");
    cache.get("a");

    cache.reset_stats();

    REQUIRE(cache.stats().hits == 0);
    REQUIRE(cache.contains("a"));
}

// ---------------------------------------------------------------------------
// Behaviour under load
// ---------------------------------------------------------------------------

TEST_CASE("stays within capacity under sustained churn", "[lru][stress]") {
    LruCache cache(64);

    for (int i = 0; i < 10'000; ++i) {
        cache.put("key" + std::to_string(i), "value" + std::to_string(i));
        REQUIRE(cache.size() <= 64);
    }

    REQUIRE(cache.size() == 64);
    REQUIRE(cache.stats().evictions == 10'000 - 64);
    // The most recent writes survive; the oldest do not.
    REQUIRE(cache.contains("key9999"));
    REQUIRE_FALSE(cache.contains("key0"));
}

TEST_CASE("byte total stays exact across mixed operations", "[lru][stress]") {
    // Byte accounting is easy to get subtly wrong, and a slow drift would only
    // show up as a memory leak in production.
    LruCache cache(1000);

    for (int i = 0; i < 500; ++i) {
        cache.put("k" + std::to_string(i), big(10));
    }
    for (int i = 0; i < 250; ++i) {
        cache.erase("k" + std::to_string(i));
    }

    std::size_t expected = 0;
    for (int i = 250; i < 500; ++i) {
        expected += ("k" + std::to_string(i)).size() + 10;
    }
    REQUIRE(cache.bytes() == expected);
}
