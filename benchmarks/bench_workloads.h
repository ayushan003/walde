#pragma once

// ─── Benchmark Workload Generator ───────────────────────────
//
// Extends the core WorkloadGenerator with YCSB-standard workloads
// and trace-file replay for externally valid benchmarking.
//
// YCSB workload definitions:
//   A: 50% read, 50% update  (write-heavy)
//   B: 95% read, 5% update   (read-mostly)
//   C: 100% read              (read-only)
//   D: read latest            (newest items read most)
//   F: read-modify-write      (50% read, 50% RMW)
//
// Trace-based workloads:
//   ARC format: one key per line (sequence of access keys).
//   Generic:    same format, any source.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace bench {

// ─── Operation ──────────────────────────────────────────────
struct BenchOp {
    enum Type { GET, PUT, RMW } type = GET;
    std::string key;
    std::string value;  // Only meaningful for PUT/RMW
};

// ─── YCSB Workload Types ────────────────────────────────────
enum class YCSBWorkload { A, B, C, D, F };

inline const char* ycsb_name(YCSBWorkload w) {
    switch (w) {
        case YCSBWorkload::A: return "YCSB-A (50r/50w)";
        case YCSBWorkload::B: return "YCSB-B (95r/5w)";
        case YCSBWorkload::C: return "YCSB-C (100r)";
        case YCSBWorkload::D: return "YCSB-D (read-latest)";
        case YCSBWorkload::F: return "YCSB-F (read-modify-write)";
    }
    return "Unknown";
}

// ─── Zipfian Generator (self-contained) ─────────────────────

class BenchZipfian {
public:
    BenchZipfian(uint32_t num_keys, double alpha, uint64_t seed)
        : num_keys_(num_keys)
        , rng_(seed)
        , cdf_(num_keys)
    {
        double harmonic = 0.0;
        for (uint32_t i = 1; i <= num_keys; ++i) {
            harmonic += 1.0 / std::pow(static_cast<double>(i), alpha);
        }
        double cumulative = 0.0;
        for (uint32_t i = 0; i < num_keys; ++i) {
            cumulative += (1.0 / std::pow(static_cast<double>(i + 1), alpha)) / harmonic;
            cdf_[i] = cumulative;
        }
        cdf_[num_keys - 1] = 1.0;
    }

    uint32_t next() {
        double u = uniform_(rng_);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<uint32_t>(it - cdf_.begin());
    }

private:
    uint32_t num_keys_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::vector<double> cdf_;
};

// ─── Uniform Generator ──────────────────────────────────────

class BenchUniform {
public:
    BenchUniform(uint32_t num_keys, uint64_t seed)
        : dist_(0, num_keys - 1)
        , rng_(seed)
    {}

    uint32_t next() { return dist_(rng_); }

private:
    std::uniform_int_distribution<uint32_t> dist_;
    std::mt19937_64 rng_;
};

// ─── Latest Generator (for YCSB-D) ─────────────────────────
//
// Generates keys biased toward recently inserted items.
// Uses a Zipfian over [0, insert_count) where 0 = most recent.

class BenchLatestGen {
public:
    BenchLatestGen(uint32_t num_keys, double alpha, uint64_t seed)
        : zipf_(num_keys, alpha, seed)
        , num_keys_(num_keys)
        , insert_count_(num_keys)
    {}

    uint32_t next() {
        uint32_t offset = zipf_.next();
        // Map: 0 → most recent, higher → older
        if (offset >= insert_count_) offset = insert_count_ - 1;
        return insert_count_ - 1 - offset;
    }

    void record_insert() { ++insert_count_; }
    uint32_t insert_count() const { return insert_count_; }

private:
    BenchZipfian zipf_;
    uint32_t num_keys_;
    uint32_t insert_count_;
};

// ─── YCSB Workload Generator ────────────────────────────────

struct BenchWorkloadConfig {
    YCSBWorkload workload     = YCSBWorkload::C;
    uint32_t     num_keys     = 100000;
    double       zipf_alpha   = 0.99;
    uint64_t     seed         = 42;
    bool         use_uniform  = false;  // For failure-case test
};

class BenchWorkloadGenerator {
public:
    explicit BenchWorkloadGenerator(const BenchWorkloadConfig& cfg)
        : cfg_(cfg)
        , zipf_(cfg.num_keys, cfg.zipf_alpha, cfg.seed)
        , uniform_(cfg.num_keys, cfg.seed + 100)
        , latest_(cfg.num_keys, cfg.zipf_alpha, cfg.seed + 200)
        , rng_(cfg.seed + 300)
    {
        // Set read/write ratios based on YCSB workload type
        switch (cfg.workload) {
            case YCSBWorkload::A: read_ratio_ = 0.50; break;
            case YCSBWorkload::B: read_ratio_ = 0.95; break;
            case YCSBWorkload::C: read_ratio_ = 1.00; break;
            case YCSBWorkload::D: read_ratio_ = 0.95; break;
            case YCSBWorkload::F: read_ratio_ = 0.50; break;
        }
    }

    BenchOp next() {
        ++ops_generated_;
        BenchOp op;

        // Determine key
        uint32_t key_idx;
        if (cfg_.use_uniform) {
            key_idx = uniform_.next();
        } else if (cfg_.workload == YCSBWorkload::D) {
            key_idx = latest_.next();
        } else {
            key_idx = zipf_.next();
        }
        op.key = "key_" + std::to_string(key_idx);

        // Determine operation type
        double r = coin_(rng_);

        if (cfg_.workload == YCSBWorkload::F) {
            // RMW: 50% pure read, 50% read-modify-write
            if (r < 0.50) {
                op.type = BenchOp::GET;
            } else {
                op.type = BenchOp::RMW;
                op.value = "rmw_" + std::to_string(ops_generated_);
            }
        } else if (cfg_.workload == YCSBWorkload::D) {
            if (r < read_ratio_) {
                op.type = BenchOp::GET;
            } else {
                // Insert new key (latest semantics)
                key_idx = latest_.insert_count();
                latest_.record_insert();
                op.key   = "key_" + std::to_string(key_idx);
                op.type  = BenchOp::PUT;
                op.value = "val_" + std::to_string(ops_generated_);
            }
        } else {
            if (r < read_ratio_) {
                op.type = BenchOp::GET;
            } else {
                op.type  = BenchOp::PUT;
                op.value = "val_" + std::to_string(ops_generated_);
            }
        }

        return op;
    }

    std::vector<BenchOp> generate_batch(uint32_t count) {
        std::vector<BenchOp> ops;
        ops.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ops.push_back(next());
        }
        return ops;
    }

    uint64_t ops_generated() const { return ops_generated_; }

private:
    BenchWorkloadConfig cfg_;
    BenchZipfian        zipf_;
    BenchUniform        uniform_;
    BenchLatestGen      latest_;
    std::mt19937_64     rng_;
    double              read_ratio_ = 1.0;
    uint64_t            ops_generated_ = 0;

    std::uniform_real_distribution<double> coin_{0.0, 1.0};
};

// ─── Trace Loader ───────────────────────────────────────────
//
// Loads access traces from files. Supports:
//   - ARC trace format (one key per line)
//   - Generic key logs (one key per line)
//
// Returns a vector of BenchOp (all GETs for trace replay).

inline std::vector<BenchOp> load_trace(const std::string& path) {
    std::vector<BenchOp> ops;
    std::ifstream in(path);
    if (!in.is_open()) return ops;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Trim whitespace
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r\n");

        std::string trimmed = line.substr(start, end - start + 1);

        // Extract first whitespace-delimited field as the key.
        // This handles both single-column (one key per line) and
        // multi-column formats like ARC traces (address size ...).
        auto space_pos = trimmed.find_first_of(" \t");
        std::string key = (space_pos != std::string::npos)
            ? trimmed.substr(0, space_pos)
            : trimmed;

        if (key.empty()) continue;

        BenchOp op;
        op.type = BenchOp::GET;
        op.key  = std::move(key);
        ops.push_back(std::move(op));
    }


    return ops;
}

// ─── Scan Workload (for scan resistance testing) ────────────

struct ScanWorkloadConfig {
    uint32_t num_keys          = 100000;
    double   zipf_alpha        = 0.99;
    double   scan_fraction     = 0.10;  // 10% of ops are scans
    uint32_t scan_length       = 500;
    double   write_ratio       = 0.05;
    uint64_t seed              = 42;
};

class ScanWorkloadGenerator {
public:
    explicit ScanWorkloadGenerator(const ScanWorkloadConfig& cfg)
        : cfg_(cfg)
        , zipf_(cfg.num_keys, cfg.zipf_alpha, cfg.seed)
        , rng_(cfg.seed + 500)
    {
        // Adjust trigger probability so that total scan ops ≈ scan_fraction
        if (cfg_.scan_fraction > 0.0 && cfg_.scan_fraction < 1.0) {
            double R = cfg_.scan_fraction;
            double S = static_cast<double>(cfg_.scan_length);
            scan_trigger_prob_ = R / (S * (1.0 - R) + R);
        }
    }

    BenchOp next() {
        ++ops_generated_;

        // In the middle of a scan burst
        if (scan_remaining_ > 0) {
            --scan_remaining_;
            BenchOp op;
            op.type = BenchOp::GET;
            op.key  = "scan_" + std::to_string(scan_offset_ + scan_remaining_);
            return op;
        }

        // Check if we should start a scan burst
        if (coin_(rng_) < scan_trigger_prob_) {
            scan_remaining_ = cfg_.scan_length;
            scan_offset_ = static_cast<uint32_t>(
                coin_(rng_) * cfg_.num_keys * 10);
            return next();
        }

        // Normal Zipfian operation
        BenchOp op;
        op.key = "key_" + std::to_string(zipf_.next());
        if (coin_(rng_) < cfg_.write_ratio) {
            op.type  = BenchOp::PUT;
            op.value = "val_" + std::to_string(ops_generated_);
        } else {
            op.type = BenchOp::GET;
        }
        return op;
    }

    std::vector<BenchOp> generate_batch(uint32_t count) {
        std::vector<BenchOp> ops;
        ops.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ops.push_back(next());
        }
        return ops;
    }

private:
    ScanWorkloadConfig cfg_;
    BenchZipfian       zipf_;
    std::mt19937_64    rng_;
    double             scan_trigger_prob_ = 0.0;
    uint32_t           scan_remaining_    = 0;
    uint32_t           scan_offset_       = 0;
    uint64_t           ops_generated_     = 0;

    std::uniform_real_distribution<double> coin_{0.0, 1.0};
};

}  // namespace bench
