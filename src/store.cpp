#include "swiftkv/store.hpp"

#include <functional>

namespace swiftkv {

std::size_t Store::round_up_pow2(std::size_t n) noexcept {
    if (n <= 1) {
        return 1;
    }
    std::size_t power = 1;
    while (power < n) {
        power <<= 1;
    }
    return power;
}

Store::Store(Config config) {
    const std::size_t count = round_up_pow2(config.shards);
    shard_mask_ = count - 1;

    shards_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        shards_.push_back(std::make_unique<Shard>(config.max_entries_per_shard,
                                                  config.max_bytes_per_shard));
    }
}

std::size_t Store::shard_index(const std::string& key) const noexcept {
    const std::size_t h = std::hash<std::string>{}(key);

    // libstdc++ hashes strings with MurmurHash, whose low bits are already well
    // mixed -- but taking the low bits directly ties shard choice to whatever
    // the standard library happens to do. Folding the high half down first
    // keeps the distribution even if that implementation ever changes, and
    // costs one xor and one shift.
    const std::size_t mixed = h ^ (h >> 32);

    // Mask rather than modulo: the shard count is a power of two, so this is a
    // single AND instead of a division on the hot path of every operation.
    return mixed & shard_mask_;
}

std::optional<std::string> Store::get(const std::string& key) {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.cache.get(key);
}

std::optional<std::string> Store::peek(const std::string& key) const {
    const Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.cache.peek(key);
}

void Store::set(const std::string& key, std::string value) {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.cache.put(key, std::move(value));
}

bool Store::del(const std::string& key) {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.cache.erase(key);
}

bool Store::contains(const std::string& key) const {
    const Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.cache.contains(key);
}

void Store::clear() {
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        shard->cache.clear();
    }
}

std::size_t Store::size() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        total += shard->cache.size();
    }
    return total;
}

Store::Stats Store::stats() const {
    Stats total;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        const auto& s = shard->cache.stats();
        total.keys += shard->cache.size();
        total.bytes += shard->cache.bytes();
        total.hits += s.hits;
        total.misses += s.misses;
        total.evictions += s.evictions;
    }
    return total;
}

void Store::reset_stats() {
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        shard->cache.reset_stats();
    }
}

std::size_t Store::shard_size(std::size_t index) const {
    const Shard& shard = *shards_.at(index);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.cache.size();
}

}  // namespace swiftkv
