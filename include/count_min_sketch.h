#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace walde {

// ─── CountMinSketch ─────────────────────────────────────────
//
// Probabilistic frequency counter using d independent hash functions
// over w counters each. Estimate = min across all rows (hence "min").
//
// Error bound: with probability 1 - δ, estimate ≤ true_count + ε·N,
// where ε = e/w, δ = (1/e)^d, N = total increments.
// With d=4, w=8192: ε≈0.00033, δ≈0.018.
//
// Decay (halving):
//   All counters are right-shifted periodically so that old
//   frequencies fade. This allows the sketch to track current
//   workload patterns rather than lifetime counts.
//   Decay is triggered by the owner (CacheStripe) every
//   5 × capacity increments.
//
// This is used as the frequency oracle in W-LFU admission:
// a window candidate replaces a probation victim only if
// its estimated frequency strictly exceeds the victim's.

class CountMinSketch {
public:
    CountMinSketch(uint32_t depth, uint32_t width);

    // Increment all rows for the given key.
    void increment(const std::string& key);

    // Return the minimum counter across all rows (frequency estimate).
    uint32_t estimate(const std::string& key) const;

    // Halve all counters (aging/decay step).
    void decay();

    // Reset all counters and the increment counter.
    void reset();

    // Total lifetime increments (used by owner to schedule decay).
    uint64_t total_increments() const { return total_increments_; }

private:
    uint32_t hash(const std::string& key, uint32_t row) const;

    uint32_t              depth_;
    uint32_t              width_;
    std::vector<uint32_t> counters_;   // depth × width, row-major
    std::vector<uint32_t> seeds_;      // Per-row hash seeds
    uint64_t              total_increments_ = 0;
};

}  // namespace walde
