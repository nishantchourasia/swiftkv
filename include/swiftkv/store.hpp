#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "swiftkv/lru.hpp"

namespace swiftkv {

/// A thread-safe key-value store built from independently locked shards.
///
/// ### Why shards
///
/// The obvious design -- one hash map behind one mutex -- is correct and
/// useless under load. Every client, whatever key it touches, queues on the
/// same lock, so the store runs at the speed of a single core no matter how
/// many are available. On the 512-core machine this was developed on, that
/// would waste essentially the whole machine.
///
/// Instead the key space is split into N independent shards, each with its own
/// map and its own lock. Two clients touching different keys almost always
/// touch different shards and never wait for one another. Contention falls
/// roughly in proportion to the shard count.
///
/// ### Why a plain mutex rather than a reader-writer lock
///
/// A `std::shared_mutex` would be the natural choice if reads were read-only.
/// They are not: a `GET` promotes the key to most-recently-used, which relinks
/// the recency list. That is a write. Since every operation mutates shard
/// state, a shared lock could never actually be taken in shared mode, and it
/// would cost more than a plain mutex for no benefit.
///
/// `peek()` exists for the cases that genuinely are read-only -- metrics and
/// replication -- and those do use a shared lock.
///
/// ### Why shards are cache-line aligned
///
/// Two mutexes sharing one 64-byte cache line cause false sharing: a core
/// locking shard 0 invalidates the cache line holding shard 1's mutex, so
/// unrelated threads slow each other down through the hardware even though
/// they never contend in software. Aligning each shard to its own cache line
/// removes that.
class Store {
public:
    struct Config {
        /// Number of shards. Rounded up to a power of two so that shard
        /// selection is a mask rather than a division.
        std::size_t shards = 16;

        /// Per-shard entry limit. The store's total capacity is this times
        /// the shard count.
        std::size_t max_entries_per_shard = 100'000;

        /// Per-shard byte limit; 0 means unbounded.
        std::size_t max_bytes_per_shard = 0;
    };

    struct Stats {
        std::size_t keys = 0;
        std::size_t bytes = 0;
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t evictions = 0;

        [[nodiscard]] double hit_rate() const noexcept {
            const std::size_t total = hits + misses;
            return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
        }
    };

    /// Construct with default configuration.
    Store() : Store(Config{}) {}

    // Written as a separate overload rather than `Store(Config = {})`: gcc 11
    // rejects a brace-init default argument on an explicit constructor.
    explicit Store(Config config);

    // Non-copyable, non-movable: shards own mutexes.
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Retrieve a key, marking it as recently used.
    std::optional<std::string> get(const std::string& key);

    /// Retrieve a key without affecting its recency.
    std::optional<std::string> peek(const std::string& key) const;

    /// Insert or overwrite a key.
    void set(const std::string& key, std::string value);

    /// Remove a key. Returns true if it existed.
    bool del(const std::string& key);

    /// Whether a key is present, without affecting recency.
    [[nodiscard]] bool contains(const std::string& key) const;

    /// Remove everything from every shard.
    void clear();

    /// Total number of keys across all shards.
    ///
    /// Locks each shard in turn rather than all at once, so this is a
    /// near-instantaneous sample rather than a consistent snapshot: keys may
    /// be added to a shard already counted. That is the right trade for a
    /// metric -- holding every lock simultaneously would stall the whole store
    /// just to answer a dashboard query.
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] bool empty() const { return size() == 0; }

    /// Aggregated statistics across shards. Sampled, not a snapshot -- see size().
    [[nodiscard]] Stats stats() const;

    void reset_stats();

    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

    /// Which shard a key belongs to. Exposed for tests and for reasoning about
    /// key distribution.
    [[nodiscard]] std::size_t shard_index(const std::string& key) const noexcept;

    /// Number of keys held by one shard, for distribution checks.
    [[nodiscard]] std::size_t shard_size(std::size_t index) const;

    /// Visit every key/value pair in the store.
    ///
    /// Locks one shard at a time, so this is **not** a consistent snapshot:
    /// concurrent writes to an already-visited shard will not be seen. Holding
    /// every lock at once would give a true snapshot at the cost of stopping
    /// the entire store, which is unacceptable for the caller this exists for
    /// (log rewriting) where a slightly stale view is harmless -- the ongoing
    /// writes are already being appended to the log separately.
    ///
    /// `fn` is called while a shard lock is held, so it must not call back into
    /// the store.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard->mutex);
            shard->cache.for_each(fn);
        }
    }

private:
    /// One shard: a cache and the lock that guards it, alone on a cache line.
    struct alignas(64) Shard {
        mutable std::mutex mutex;
        LruCache cache;

        Shard(std::size_t max_entries, std::size_t max_bytes)
            : cache(max_entries, max_bytes) {}
    };

    static std::size_t round_up_pow2(std::size_t n) noexcept;

    Shard& shard_for(const std::string& key) noexcept {
        return *shards_[shard_index(key)];
    }
    const Shard& shard_for(const std::string& key) const noexcept {
        return *shards_[shard_index(key)];
    }

    // unique_ptr because Shard holds a mutex and so cannot be moved, which a
    // vector needs to be able to do when it grows.
    std::vector<std::unique_ptr<Shard>> shards_;
    std::size_t shard_mask_ = 0;
};

}  // namespace swiftkv
