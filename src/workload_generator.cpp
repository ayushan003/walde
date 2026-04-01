#include "workload_generator.h"

#include <algorithm>
#include <cmath>

namespace walde {

// ─── ZipfianGenerator ───────────────────────────────────────

ZipfianGenerator::ZipfianGenerator(uint32_t num_keys, double alpha,
                                   uint64_t seed)
    : num_keys_(num_keys)
    , alpha_(alpha)
    , rng_(seed)
    , cdf_(num_keys)
{
    // Precompute the CDF.
    // PMF for Zipf: P(k) = (1/k^alpha) / H_N,alpha
    // where H_N,alpha = sum_{i=1}^{N} 1/i^alpha (generalized harmonic)
    double harmonic = 0.0;
    for (uint32_t i = 1; i <= num_keys; ++i) {
        harmonic += 1.0 / std::pow(static_cast<double>(i), alpha);
    }

    double cumulative = 0.0;
    for (uint32_t i = 0; i < num_keys; ++i) {
        cumulative += (1.0 / std::pow(static_cast<double>(i + 1), alpha)) / harmonic;
        cdf_[i] = cumulative;
    }
    cdf_[num_keys - 1] = 1.0;  // Ensure last element is exactly 1.0
}

uint32_t ZipfianGenerator::next_index() {
    double u = uniform_(rng_);
    // Binary search in the CDF — O(log N)
    auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
    return static_cast<uint32_t>(it - cdf_.begin());
}

std::string ZipfianGenerator::next_key() {
    return "key_" + std::to_string(next_index());
}

// ─── WorkloadGenerator ──────────────────────────────────────

WorkloadGenerator::WorkloadGenerator(const Config& config)
    : config_(config)
    , zipf_(config.num_keys, config.zipf_alpha, config.seed)
    , rng_(config.seed + 1)  // Different seed from Zipfian
{
    // Convert target volume ratio (e.g., 10%) to actual trigger probability
    if (config_.scan_probability > 0.0 && config_.scan_probability < 1.0) {
        double R = config_.scan_probability;  // e.g., 0.10
        double S = static_cast<double>(config_.scan_length); // e.g., 500.0
        
        actual_scan_prob_ = R / (S * (1.0 - R) + R);
    } else {
        actual_scan_prob_ = config_.scan_probability;
    }
}

WorkloadOp WorkloadGenerator::next() {
    ++ops_generated_;

    // If we're in the middle of a scan burst, emit sequential keys
    if (scan_remaining_ > 0) {
        --scan_remaining_;
        std::string key = "scan_" + std::to_string(scan_offset_ + scan_remaining_);

        WorkloadOp op;
        op.type  = WorkloadOp::GET;
        op.key   = std::move(key);
        return op;
    }

    // Check if we should start a scan burst using the mathematically corrected probability
    if (uniform_(rng_) < actual_scan_prob_) {
        scan_remaining_ = config_.scan_length;
        // Random offset so scans hit different parts of the key space
        scan_offset_ = static_cast<uint32_t>(
            uniform_(rng_) * config_.num_keys * 10);
        return next();  // Recurse to emit first scan key
    }

    // Normal Zipfian operation
    WorkloadOp op;
    op.key = zipf_.next_key();

    if (uniform_(rng_) < config_.write_ratio) {
        op.type  = WorkloadOp::PUT;
        op.value = "val_" + std::to_string(ops_generated_);
    } else {
        op.type = WorkloadOp::GET;
    }

    return op;
}

std::vector<WorkloadOp> WorkloadGenerator::generate_batch(uint32_t count) {
    std::vector<WorkloadOp> ops;
    ops.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ops.push_back(next());
    }
    return ops;
}

}  // namespace walde
