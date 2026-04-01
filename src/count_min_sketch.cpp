#include "count_min_sketch.h"

#include <algorithm>
#include <cassert>
#include <xxhash.h>

namespace walde {

CountMinSketch::CountMinSketch(uint32_t depth, uint32_t width)
    : depth_(depth)
    , width_(width)
    , counters_(static_cast<size_t>(depth) * width, 0)
    , seeds_(depth)
{
    assert(depth > 0 && "CMS depth must be > 0");
    assert(width > 0 && "CMS width must be > 0");

    // Deterministic seeds: different per row, reproducible across runs.
    // Using primes spaced apart to minimize hash correlation.
    for (uint32_t i = 0; i < depth; ++i) {
        seeds_[i] = 0x9E3779B9u + i * 0x6C62272Eu;
    }
}

void CountMinSketch::increment(const std::string& key) {
    for (uint32_t row = 0; row < depth_; ++row) {
        uint32_t col = hash(key, row);
        ++counters_[row * width_ + col];
    }
    ++total_increments_;
}

uint32_t CountMinSketch::estimate(const std::string& key) const {
    uint32_t min_val = UINT32_MAX;

    for (uint32_t row = 0; row < depth_; ++row) {
        uint32_t col = hash(key, row);
        min_val = std::min(min_val, counters_[row * width_ + col]);
    }

    return min_val;
}

void CountMinSketch::decay() {
    // Halve all counters by right-shifting.
    // This adapts the sketch to workload changes: old frequencies
    // fade, recent frequencies dominate. Without decay, a key that
    // was hot an hour ago but is cold now would still have a high
    // count, blocking admission of genuinely hot new keys.
    for (auto& counter : counters_) {
        counter >>= 1;
    }

    // Don't reset total_increments_ — it tracks lifetime ops
    // for the caller to know when to trigger decay.
}

void CountMinSketch::reset() {
    std::fill(counters_.begin(), counters_.end(), 0);
    total_increments_ = 0;
}

uint32_t CountMinSketch::hash(const std::string& key, uint32_t row) const {
    // XXHash32 with per-row seed for independence.
    // XXHash is fast (< 10ns per call) and has excellent distribution.
    uint32_t h = XXH32(key.data(), key.size(), seeds_[row]);
    return h % width_;
}

}  // namespace walde
