#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace walde {

// ─── WorkloadOp ─────────────────────────────────────────────
struct WorkloadOp {
    enum Type { GET, PUT } type = GET;
    std::string key;
    std::string value;  // Only set for PUT
};

// ─── ZipfianGenerator ───────────────────────────────────────
//
// Generates keys with Zipf(α) distribution over [0, num_keys).
// Uses precomputed CDF + binary search: O(log N) per sample.
// For α=0.99 with 100K keys, the top 1% of keys receive ~75% of accesses.

class ZipfianGenerator {
public:
    ZipfianGenerator(uint32_t num_keys, double alpha, uint64_t seed);

    uint32_t    next_index();
    std::string next_key();

private:
    uint32_t                   num_keys_;
    double                     alpha_;
    std::mt19937_64            rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::vector<double>        cdf_;
};

// ─── WorkloadGenerator ──────────────────────────────────────
//
// Generates a mixed read/write workload with optional scan bursts.
//
// Scan injection:
//   scan_probability is the TARGET fraction of total ops that are
//   scan ops. The actual trigger probability is adjusted to account
//   for the burst length, so the resulting mix is accurate.
//
//   Example: scan_probability=0.10, scan_length=500 means 10% of
//   total ops will be sequential scan reads.

class WorkloadGenerator {
public:
    struct Config {
        uint32_t num_keys        = 10000;
        double   zipf_alpha      = 0.99;
        double   write_ratio     = 0.05;
        double   scan_probability = 0.0;
        uint32_t scan_length     = 500;
        uint64_t seed            = 42;
    };

    explicit WorkloadGenerator(const Config& config);

    WorkloadOp              next();
    std::vector<WorkloadOp> generate_batch(uint32_t count);

    uint64_t ops_generated() const { return ops_generated_; }

private:
    Config           config_;
    ZipfianGenerator zipf_;
    std::mt19937_64  rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    double           actual_scan_prob_ = 0.0;
    uint32_t         scan_remaining_   = 0;
    uint32_t         scan_offset_      = 0;
    uint64_t         ops_generated_    = 0;
};

}  // namespace walde
