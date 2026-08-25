#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace swiftkv {

/// A least-recently-used cache mapping string keys to string values.
///
/// Every operation is O(1). Two structures cooperate to achieve that:
///
///   * a `std::list` holding entries in recency order, most-recent first;
///   * a hash map from key to that entry's position in the list.
///
/// The list is what makes eviction cheap -- the least-recently-used entry is
/// always the last one, so evicting is a pop rather than a search. The map is
/// what makes lookup cheap. Recording a use is a `splice`, which relinks a
/// node without copying or invalidating it; this is the reason a `std::list`
/// is used rather than a vector or deque.
///
/// Capacity is bounded two ways, and whichever binds first wins:
///
///   * `max_entries` -- a count of keys;
///   * `max_bytes`   -- the total size of keys plus values, which is what
///                      actually protects a server from being driven out of
///                      memory by a few very large values.
///
/// This class performs **no locking**. It is a single-threaded building block;
/// `Store` is what shards it and adds synchronisation. Keeping the lock out of
/// here means the cache can be tested deterministically, without threads.
class LruCache {
public:
    struct Stats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t evictions = 0;
        std::size_t expired = 0;
    };

    /// Construct a cache bounded by entry count and, optionally, by bytes.
    /// `max_bytes == 0` means no byte bound.
    explicit LruCache(std::size_t max_entries, std::size_t max_bytes = 0);

    /// Look a key up, promoting it to most-recently-used on a hit.
    std::optional<std::string> get(const std::string& key);

    /// Look a key up **without** changing its recency.
    ///
    /// Needed by replication and by the dashboard: a background reader should
    /// not be able to keep a key alive that no client is actually using.
    std::optional<std::string> peek(const std::string& key) const;

    /// Insert or overwrite a key, evicting as needed to stay within capacity.
    void put(std::string key, std::string value);

    /// Remove a key. Returns true if it was present.
    bool erase(const std::string& key);

    /// Remove everything.
    void clear();

    [[nodiscard]] bool contains(const std::string& key) const noexcept {
        return index_.find(key) != index_.end();
    }

    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

    /// Bytes currently held, counting keys and values.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = Stats{}; }

    /// Hit rate over this cache's lifetime, or 0 if nothing has been looked up.
    [[nodiscard]] double hit_rate() const noexcept;

    /// The least-recently-used key, for tests and introspection.
    [[nodiscard]] std::optional<std::string> lru_key() const;

private:
    struct Entry {
        std::string key;
        std::string value;
    };

    using List = std::list<Entry>;

    static std::size_t entry_bytes(const std::string& key, const std::string& value) noexcept {
        return key.size() + value.size();
    }

    /// Evict from the back until both bounds are satisfied.
    void evict_to_fit();

    List entries_;
    std::unordered_map<std::string, List::iterator> index_;

    std::size_t max_entries_;
    std::size_t max_bytes_;
    std::size_t bytes_ = 0;
    Stats stats_;
};

}  // namespace swiftkv
