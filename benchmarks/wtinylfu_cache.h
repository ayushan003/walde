#pragma once

// ─── W-TinyLFU Cache ────────────────────────────────────────
//
// Faithful implementation of the W-TinyLFU admission policy
// as described in the Caffeine/Ben-Manes paper.
//
// Components:
//   1. Count-Min Sketch (CMS): frequency estimator (same depth=4,
//      width=8192 as WALDE's CMS for fair comparison).
//   2. Bloom filter doorkeeper: blocks first-time entries from
//      inflating CMS counters. Reset when CMS decays.
//   3. Window LRU (1% of capacity): new items enter here first.
//   4. Main cache split into:
//      - Probation LRU (20% of main): candidates from window.
//      - Protected LRU (80% of main): promoted from probation on hit.
//
// Admission rule (window → main):
//   When window overflows, candidate is admitted to probation only
//   if its CMS frequency exceeds the probation LRU victim's frequency.
//   The Bloom doorkeeper must have seen the key at least once before
//   the CMS increment takes effect.
//
// Thread safety: single mutex (same as LRU baseline).
// Same key/value types (std::string) as WALDE.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bench {

// ─── Bloom Filter Doorkeeper ────────────────────────────────
//
// Simple Bloom filter that tracks whether a key has been seen
// at least once since the last reset. This prevents one-hit
// wonders from polluting the CMS frequency counters.
//
// Uses 2 hash functions over a bit array. False positive rate
// is acceptable since the consequence is only a premature CMS
// increment, not data corruption.

class BloomDoorkeeper {
public:
    explicit BloomDoorkeeper(uint32_t num_bits = 65536)
        : num_bits_(num_bits)
        , bits_((num_bits + 63) / 64, 0)
    {}

    // Returns true if key was already present (or false positive).
    // Always marks the key as present.
    bool contains_and_add(const std::string& key) {
        uint32_t h1 = hash1(key);
        uint32_t h2 = hash2(key);

        bool was_present = test_bit(h1) && test_bit(h2);

        set_bit(h1);
        set_bit(h2);

        return was_present;
    }

    bool contains(const std::string& key) const {
        return test_bit(hash1(key)) && test_bit(hash2(key));
    }

    void reset() {
        std::fill(bits_.begin(), bits_.end(), 0);
    }

private:
    uint32_t hash1(const std::string& key) const {
        // FNV-1a variant
        uint32_t h = 0x811C9DC5u;
        for (char c : key) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x01000193u;
        }
        return h % num_bits_;
    }

    uint32_t hash2(const std::string& key) const {
        // djb2 variant
        uint32_t h = 5381;
        for (char c : key) {
            h = ((h << 5) + h) + static_cast<uint8_t>(c);
        }
        return h % num_bits_;
    }

    bool test_bit(uint32_t pos) const {
        return (bits_[pos / 64] >> (pos % 64)) & 1;
    }

    void set_bit(uint32_t pos) {
        bits_[pos / 64] |= (1ULL << (pos % 64));
    }

    uint32_t num_bits_;
    std::vector<uint64_t> bits_;
};

// ─── Count-Min Sketch (standalone for W-TinyLFU) ───────────
//
// Same depth=4, width=8192 as WALDE's CMS. Uses different hash
// seeds from the Bloom filter to maintain independence.

class TinyLFUSketch {
public:
    TinyLFUSketch(uint32_t depth = 4, uint32_t width = 8192)
        : depth_(depth)
        , width_(width)
        , counters_(static_cast<size_t>(depth) * width, 0)
        , seeds_(depth)
    {
        for (uint32_t i = 0; i < depth; ++i) {
            seeds_[i] = 0x9E3779B9u + i * 0x6C62272Eu;
        }
    }

    void increment(const std::string& key) {
        for (uint32_t row = 0; row < depth_; ++row) {
            uint32_t col = hash(key, row);
            ++counters_[row * width_ + col];
        }
        ++total_increments_;
    }

    uint32_t estimate(const std::string& key) const {
        uint32_t min_val = UINT32_MAX;
        for (uint32_t row = 0; row < depth_; ++row) {
            uint32_t col = hash(key, row);
            min_val = std::min(min_val, counters_[row * width_ + col]);
        }
        return min_val;
    }

    void decay() {
        for (auto& c : counters_) c >>= 1;
    }

    void reset() {
        std::fill(counters_.begin(), counters_.end(), 0);
        total_increments_ = 0;
    }

    uint64_t total_increments() const { return total_increments_; }

private:
    uint32_t hash(const std::string& key, uint32_t row) const {
        // Same hash as WALDE's CMS for fairness, but we use a
        // simple multiplicative hash rather than XXHash to avoid
        // the dependency. The quality difference is negligible
        // for frequency estimation at this width.
        uint32_t h = seeds_[row];
        for (char c : key) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x01000193u;  // FNV prime
            h ^= h >> 16;
        }
        return h % width_;
    }

    uint32_t depth_;
    uint32_t width_;
    std::vector<uint32_t> counters_;
    std::vector<uint32_t> seeds_;
    uint64_t total_increments_ = 0;
};

// ─── W-TinyLFU Cache ────────────────────────────────────────

class WTinyLFUCache {
public:
    explicit WTinyLFUCache(uint32_t capacity,
                           float window_pct    = 0.01f,
                           float probation_pct = 0.20f)
        : total_capacity_(capacity)
        , doorkeeper_(capacity * 8)  // ~8 bits per entry
        , cms_(4, 8192)
    {
        window_capacity_ = std::max(3u, static_cast<uint32_t>(capacity * window_pct));
        if (window_capacity_ >= capacity - 2) {
            window_capacity_ = capacity / 4;
        }

        uint32_t main_capacity = capacity - window_capacity_;
        probation_capacity_ = std::max(1u, static_cast<uint32_t>(main_capacity * probation_pct));
        protected_capacity_ = main_capacity - probation_capacity_;

        decay_interval_ = static_cast<uint64_t>(capacity) * 5;
        if (decay_interval_ == 0) decay_interval_ = 1;

        map_.reserve(capacity);
    }

    WTinyLFUCache(const WTinyLFUCache&) = delete;
    WTinyLFUCache& operator=(const WTinyLFUCache&) = delete;

    std::optional<std::string> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it == map_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        // Record access in CMS (doorkeeper already saw this key on insert)
        record_access(key);

        Entry& entry = *it->second;
        switch (entry.segment) {
            case Seg::Window:
                window_.splice(window_.begin(), window_, it->second);
                break;
            case Seg::Probation:
                promote_to_protected(it->second);
                break;
            case Seg::Protected:
                protected_.splice(protected_.begin(), protected_, it->second);
                break;
        }

        hits_.fetch_add(1, std::memory_order_relaxed);
        return entry.value;
    }

    bool put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Record access through doorkeeper + CMS
        record_access(key);

        auto it = map_.find(key);
        if (it != map_.end()) {
            Entry& entry = *it->second;
            entry.value = value;
            switch (entry.segment) {
                case Seg::Window:
                    window_.splice(window_.begin(), window_, it->second);
                    break;
                case Seg::Probation:
                    promote_to_protected(it->second);
                    break;
                case Seg::Protected:
                    protected_.splice(protected_.begin(), protected_, it->second);
                    break;
            }
            return false;
        }

        // Insert new entry into window
        window_.push_front({key, value, Seg::Window});
        map_[key] = window_.begin();

        // Overflow: try to admit window LRU into main
        while (window_.size() > window_capacity_) {
            try_admit_from_window();
        }

        return true;
    }

    // ── Stats ──────────────────────────────────────────────
    uint64_t hits()       const { return hits_.load(std::memory_order_relaxed); }
    uint64_t misses()     const { return misses_.load(std::memory_order_relaxed); }
    uint64_t evictions()  const { return evictions_.load(std::memory_order_relaxed); }
    uint64_t admissions() const { return admissions_.load(std::memory_order_relaxed); }
    uint64_t rejections() const { return rejections_.load(std::memory_order_relaxed); }

    uint32_t size() const {
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
        admissions_.store(0, std::memory_order_relaxed);
        rejections_.store(0, std::memory_order_relaxed);
    }

private:
    enum class Seg : uint8_t { Window, Probation, Protected };

    struct Entry {
        std::string key;
        std::string value;
        Seg segment;
    };

    using ListIt = std::list<Entry>::iterator;

    void record_access(const std::string& key) {
        // Bloom doorkeeper: only increment CMS if the key has been
        // seen before. This prevents one-hit-wonders from inflating
        // frequency counts.
        bool seen_before = doorkeeper_.contains_and_add(key);
        if (seen_before) {
            cms_.increment(key);
        }
        maybe_decay();
    }

    void maybe_decay() {
        ++ops_since_decay_;
        if (ops_since_decay_ >= decay_interval_) {
            cms_.decay();
            doorkeeper_.reset();
            ops_since_decay_ = 0;
        }
    }

    void try_admit_from_window() {
        if (window_.empty()) return;

        // Pop LRU from window
        auto candidate_it = std::prev(window_.end());
        std::string cand_key = candidate_it->key;
        std::string cand_val = candidate_it->value;

        uint32_t main_size = static_cast<uint32_t>(
            probation_.size() + protected_.size());

        if (main_size < probation_capacity_ + protected_capacity_) {
            // Main has room — admit directly
            map_.erase(cand_key);
            window_.erase(candidate_it);

            probation_.push_front({cand_key, cand_val, Seg::Probation});
            map_[cand_key] = probation_.begin();
            admissions_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (probation_.empty()) {
            // No victim — reject candidate
            map_.erase(cand_key);
            window_.erase(candidate_it);
            rejections_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Admission gate: compare frequencies
        auto victim_it = std::prev(probation_.end());
        uint32_t cand_freq   = cms_.estimate(cand_key);
        uint32_t victim_freq = cms_.estimate(victim_it->key);

        if (cand_freq > victim_freq) {
            // Admit candidate, evict victim
            std::string victim_key = victim_it->key;
            map_.erase(victim_key);
            probation_.erase(victim_it);
            evictions_.fetch_add(1, std::memory_order_relaxed);

            map_.erase(cand_key);
            window_.erase(candidate_it);

            probation_.push_front({cand_key, cand_val, Seg::Probation});
            map_[cand_key] = probation_.begin();
            admissions_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Reject candidate
            map_.erase(cand_key);
            window_.erase(candidate_it);
            rejections_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void promote_to_protected(ListIt it) {
        Entry entry = std::move(*it);
        map_.erase(entry.key);
        probation_.erase(it);

        // If protected is full, demote its LRU to probation
        if (protected_.size() >= protected_capacity_) {
            auto demoted_it = std::prev(protected_.end());
            demoted_it->segment = Seg::Probation;
            std::string dk = demoted_it->key;
            probation_.splice(probation_.begin(), protected_, demoted_it);
            map_[dk] = probation_.begin();
        }

        entry.segment = Seg::Protected;
        protected_.push_front(std::move(entry));
        map_[protected_.begin()->key] = protected_.begin();
    }

    // ── Data ──────────────────────────────────────────────
    uint32_t total_capacity_;
    uint32_t window_capacity_;
    uint32_t probation_capacity_;
    uint32_t protected_capacity_;

    mutable std::mutex mutex_;

    std::list<Entry> window_;
    std::list<Entry> probation_;
    std::list<Entry> protected_;

    std::unordered_map<std::string, ListIt> map_;

    BloomDoorkeeper doorkeeper_;
    TinyLFUSketch   cms_;
    uint64_t        decay_interval_;
    uint64_t        ops_since_decay_ = 0;

    std::atomic<uint64_t> hits_       {0};
    std::atomic<uint64_t> misses_     {0};
    std::atomic<uint64_t> evictions_  {0};
    std::atomic<uint64_t> admissions_ {0};
    std::atomic<uint64_t> rejections_ {0};
};

}  // namespace bench
