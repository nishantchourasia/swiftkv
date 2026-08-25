#include "swiftkv/lru.hpp"

#include <algorithm>

namespace swiftkv {

LruCache::LruCache(std::size_t max_entries, std::size_t max_bytes)
    : max_entries_(max_entries), max_bytes_(max_bytes) {
    // A zero-entry cache would evict everything it was given, which is almost
    // certainly a configuration mistake rather than an intent. Treat it as a
    // cache of one so the store still functions.
    if (max_entries_ == 0) {
        max_entries_ = 1;
    }
}

std::optional<std::string> LruCache::get(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }
    ++stats_.hits;
    // Promote to most-recently-used. splice relinks the node in place: no
    // allocation, no copy, and the iterator held in index_ stays valid.
    entries_.splice(entries_.begin(), entries_, it->second);
    return it->second->value;
}

std::optional<std::string> LruCache::peek(const std::string& key) const {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }
    return it->second->value;
}

void LruCache::put(std::string key, std::string value) {
    auto it = index_.find(key);

    if (it != index_.end()) {
        // Overwrite in place, adjusting the byte total by the difference so
        // that a shrinking value releases budget rather than leaking it.
        Entry& entry = *it->second;
        bytes_ -= entry.value.size();
        bytes_ += value.size();
        entry.value = std::move(value);
        entries_.splice(entries_.begin(), entries_, it->second);
        evict_to_fit();
        return;
    }

    const std::size_t added = entry_bytes(key, value);
    entries_.push_front(Entry{key, std::move(value)});
    index_.emplace(std::move(key), entries_.begin());
    bytes_ += added;

    evict_to_fit();
}

bool LruCache::erase(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    bytes_ -= entry_bytes(it->second->key, it->second->value);
    entries_.erase(it->second);
    index_.erase(it);
    return true;
}

void LruCache::clear() {
    entries_.clear();
    index_.clear();
    bytes_ = 0;
}

void LruCache::evict_to_fit() {
    const bool byte_bound = max_bytes_ > 0;

    while (!entries_.empty() &&
           (index_.size() > max_entries_ || (byte_bound && bytes_ > max_bytes_))) {
        // Never evict the only remaining entry on account of the byte bound:
        // a single value larger than the whole budget would otherwise leave the
        // cache permanently empty and every write would be silently discarded.
        if (index_.size() == 1 && index_.size() <= max_entries_) {
            break;
        }

        Entry& victim = entries_.back();
        bytes_ -= entry_bytes(victim.key, victim.value);
        index_.erase(victim.key);
        entries_.pop_back();
        ++stats_.evictions;
    }
}

double LruCache::hit_rate() const noexcept {
    const std::size_t total = stats_.hits + stats_.misses;
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(stats_.hits) / static_cast<double>(total);
}

std::optional<std::string> LruCache::lru_key() const {
    if (entries_.empty()) {
        return std::nullopt;
    }
    return entries_.back().key;
}

}  // namespace swiftkv
