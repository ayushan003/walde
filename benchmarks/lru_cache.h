#pragma once

// ─── Baseline LRU Cache ─────────────────────────────────────
//
// Single-segment LRU eviction policy. No admission gate, no
// frequency tracking. Items enter the cache unconditionally;
// on capacity overflow the least-recently-used item is evicted.
//
// This serves as the simplest baseline for comparison against
// WALDE and W-TinyLFU. Any frequency-aware policy should beat
// LRU on skewed (Zipfian) workloads.
//
// Thread safety: single mutex guards all operations.
// Same key/value types (std::string) as WALDE.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace bench {

class LRUCache {
public:
    explicit LRUCache(uint32_t capacity)
        : capacity_(capacity)
    {
        map_.reserve(capacity);
    }

    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    std::optional<std::string> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it == map_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        // Move to front (MRU position)
        list_.splice(list_.begin(), list_, it->second);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second->value;
    }

    bool put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update existing entry
            it->second->value = value;
            list_.splice(list_.begin(), list_, it->second);
            return false;  // Not a new insertion
        }

        // Evict if at capacity
        while (list_.size() >= capacity_) {
            auto& victim = list_.back();
            map_.erase(victim.key);
            list_.pop_back();
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }

        // Insert new entry at front
        list_.push_front({key, value});
        map_[key] = list_.begin();
        return true;
    }

    // ── Stats ──────────────────────────────────────────────
    uint64_t hits()      const { return hits_.load(std::memory_order_relaxed); }
    uint64_t misses()    const { return misses_.load(std::memory_order_relaxed); }
    uint64_t evictions() const { return evictions_.load(std::memory_order_relaxed); }
    uint32_t size()      const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(map_.size());
    }

    double hit_rate() const {
        uint64_t h = hits(), m = misses();
        uint64_t total = h + m;
        return (total > 0) ? static_cast<double>(h) / total : 0.0;
    }

    void reset_stats() {
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
        evictions_.store(0, std::memory_order_relaxed);
    }

private:
    struct Node {
        std::string key;
        std::string value;
    };

    uint32_t capacity_;
    mutable std::mutex mutex_;

    std::list<Node> list_;  // MRU at front, LRU at back
    std::unordered_map<std::string, std::list<Node>::iterator> map_;

    std::atomic<uint64_t> hits_      {0};
    std::atomic<uint64_t> misses_    {0};
    std::atomic<uint64_t> evictions_ {0};
};

// ─── Striped LRU Cache ──────────────────────────────────────
//
// 64-stripe LRU with no admission gate. Same sharding as WALDE
// but pure LRU eviction per stripe. This isolates the effect of
// striping from the effect of the W-LFU admission policy, giving
// a fair concurrency baseline.

class StripedLRUCache {
public:
    static constexpr uint32_t kStripes = 64;

    explicit StripedLRUCache(uint32_t total_capacity)
        : per_stripe_cap_(std::max(1u, total_capacity / kStripes))
    {
        for (uint32_t i = 0; i < kStripes; ++i) {
            stripes_[i].map.reserve(per_stripe_cap_);
        }
    }

    StripedLRUCache(const StripedLRUCache&) = delete;
    StripedLRUCache& operator=(const StripedLRUCache&) = delete;

    std::optional<std::string> get(const std::string& key) {
        auto& s = stripe_for(key);
        std::lock_guard<std::mutex> lock(s.mutex);

        auto it = s.map.find(key);
        if (it == s.map.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        s.list.splice(s.list.begin(), s.list, it->second);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second->value;
    }

    bool put(const std::string& key, const std::string& value) {
        auto& s = stripe_for(key);
        std::lock_guard<std::mutex> lock(s.mutex);

        auto it = s.map.find(key);
        if (it != s.map.end()) {
            it->second->value = value;
            s.list.splice(s.list.begin(), s.list, it->second);
            return false;
        }

        while (s.list.size() >= per_stripe_cap_) {
            auto& victim = s.list.back();
            s.map.erase(victim.key);
            s.list.pop_back();
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }

        s.list.push_front({key, value});
        s.map[key] = s.list.begin();
        return true;
    }

    uint64_t hits()      const { return hits_.load(std::memory_order_relaxed); }
    uint64_t misses()    const { return misses_.load(std::memory_order_relaxed); }
    uint64_t evictions() const { return evictions_.load(std::memory_order_relaxed); }

    double hit_rate() const {
        uint64_t h = hits(), m = misses();
        uint64_t total = h + m;
        return (total > 0) ? static_cast<double>(h) / total : 0.0;
    }

    void reset_stats() {
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
        evictions_.store(0, std::memory_order_relaxed);
    }

private:
    struct Node {
        std::string key;
        std::string value;
    };

    struct Stripe {
        std::mutex mutex;
        std::list<Node> list;
        std::unordered_map<std::string, std::list<Node>::iterator> map;
    };

    Stripe& stripe_for(const std::string& key) {
        // FNV-1a to match simple hashing (no XXHash dependency)
        uint32_t h = 0x811C9DC5u;
        for (char c : key) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x01000193u;
        }
        return stripes_[h % kStripes];
    }

    uint32_t per_stripe_cap_;
    std::array<Stripe, kStripes> stripes_;

    std::atomic<uint64_t> hits_      {0};
    std::atomic<uint64_t> misses_    {0};
    std::atomic<uint64_t> evictions_ {0};
};

}  // namespace bench
