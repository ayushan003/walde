#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace walde {

// ─── LatencyBreakdown ───────────────────────────────────────
//
// Per-request decomposition of where time is spent.
// Each field is populated by the component that owns that phase.
// All times in nanoseconds.

struct LatencyBreakdown {
    int64_t total_ns     = 0;  // End-to-end wall time
    int64_t lock_wait_ns = 0;  // Time blocked waiting for stripe mutex
    int64_t lookup_ns    = 0;  // Hash map find + LRU promotion
    int64_t admission_ns = 0;  // CMS estimate + frequency comparison
    int64_t eviction_ns  = 0;  // Victim demotion + slab dealloc
    int64_t l2_ns        = 0;  // L2 lookup time
    int64_t backend_ns   = 0;  // Storage backend access time
    int64_t slab_ns      = 0;  // Slab allocate time

    enum class Path : uint8_t {
        L1_HIT      = 0,
        L2_HIT      = 1,
        BACKEND_HIT = 2,
        MISS        = 3,
    };
    Path path = Path::MISS;
};

// ─── ScopedTimer ────────────────────────────────────────────
// RAII helper: writes elapsed nanoseconds to target on destruction.

class ScopedTimer {
public:
    explicit ScopedTimer(int64_t& target)
        : target_(target)
        , start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::steady_clock::now();
        target_  = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start_).count();
    }

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    int64_t&                                   target_;
    std::chrono::steady_clock::time_point      start_;
};

// ─── LatencyHistogram ───────────────────────────────────────
//
// Fixed-bucket log-scale histogram. 64 buckets covering ~50ns to ~50ms.
// Bucket 0: [0, 100ns). Bucket i≥1: [100·2^(i-1), 100·2^i) ns.
// Percentile returned as the bucket midpoint in microseconds.
//
// Resolution is intentionally coarse (each bucket spans 2× the previous).
// For systems benchmarking this is sufficient; the coarseness shows up
// as round-looking numbers in output (bucket midpoints).
//
// No per-record heap allocation. Merge() is O(kNumBuckets).

class LatencyHistogram {
public:
    static constexpr int kNumBuckets = 64;

    void record(int64_t ns) {
        int bucket = 0;
        if (ns >= 100) {
            bucket = 1;
            int64_t threshold = 200;
            while (bucket < kNumBuckets - 1 && ns >= threshold) {
                ++bucket;
                threshold <<= 1;
            }
        }
        ++counts_[bucket];
        ++total_count_;
        total_sum_ += ns;
    }

    double percentile_us(double p) const {
        if (total_count_ == 0) return 0.0;
        uint64_t target     = static_cast<uint64_t>(p * total_count_);
        uint64_t cumulative = 0;

        for (int i = 0; i < kNumBuckets; ++i) {
            cumulative += counts_[i];
            if (cumulative > target) {
                if (i == 0) return 0.05;  // [0, 100ns) → 50ns midpoint
                int64_t lo = 100LL << (i - 1);
                int64_t hi = 100LL << i;
                return static_cast<double>(lo + hi) / 2.0 / 1000.0;
            }
        }
        // Last bucket upper-bound guard (no 64-bit overflow: use 1ULL).
        return (100.0 * static_cast<double>(1ULL << (kNumBuckets - 2))) / 1000.0;
    }

    double   mean_us()     const {
        if (total_count_ == 0) return 0.0;
        return static_cast<double>(total_sum_) / total_count_ / 1000.0;
    }
    uint64_t count()       const { return total_count_; }

    void merge(const LatencyHistogram& other) {
        for (int i = 0; i < kNumBuckets; ++i)
            counts_[i] += other.counts_[i];
        total_count_ += other.total_count_;
        total_sum_   += other.total_sum_;
    }

    void reset() {
        std::memset(counts_, 0, sizeof(counts_));
        total_count_ = 0;
        total_sum_   = 0;
    }

private:
    uint64_t counts_[kNumBuckets] = {};
    uint64_t total_count_         = 0;
    int64_t  total_sum_           = 0;
};

// ─── PathLatencyTracker ─────────────────────────────────────
//
// Collects per-path and per-component latency histograms.
// One instance per benchmark thread; merge across threads after run.
// Not thread-safe by itself — use one per thread.

class PathLatencyTracker {
public:
    void record(const LatencyBreakdown& bd) {
        total_.record(bd.total_ns);
        lock_wait_.record(bd.lock_wait_ns);
        lookup_.record(bd.lookup_ns);
        slab_.record(bd.slab_ns);

        if (bd.admission_ns > 0) admission_.record(bd.admission_ns);
        if (bd.eviction_ns  > 0) eviction_.record(bd.eviction_ns);
        if (bd.l2_ns        > 0) l2_.record(bd.l2_ns);
        if (bd.backend_ns   > 0) backend_.record(bd.backend_ns);

        switch (bd.path) {
            case LatencyBreakdown::Path::L1_HIT:
                l1_hit_.record(bd.total_ns);     break;
            case LatencyBreakdown::Path::L2_HIT:
                l2_hit_.record(bd.total_ns);     break;
            case LatencyBreakdown::Path::BACKEND_HIT:
                backend_hit_.record(bd.total_ns); break;
            case LatencyBreakdown::Path::MISS:
                full_miss_.record(bd.total_ns);  break;
        }
    }

    void merge(const PathLatencyTracker& other) {
        total_.merge(other.total_);
        lock_wait_.merge(other.lock_wait_);
        lookup_.merge(other.lookup_);
        admission_.merge(other.admission_);
        eviction_.merge(other.eviction_);
        l2_.merge(other.l2_);
        backend_.merge(other.backend_);
        slab_.merge(other.slab_);
        l1_hit_.merge(other.l1_hit_);
        l2_hit_.merge(other.l2_hit_);
        backend_hit_.merge(other.backend_hit_);
        full_miss_.merge(other.full_miss_);
    }

    // Component histograms
    LatencyHistogram total_;
    LatencyHistogram lock_wait_;
    LatencyHistogram lookup_;
    LatencyHistogram admission_;
    LatencyHistogram eviction_;
    LatencyHistogram l2_;
    LatencyHistogram backend_;
    LatencyHistogram slab_;

    // Per-path end-to-end histograms
    LatencyHistogram l1_hit_;
    LatencyHistogram l2_hit_;
    LatencyHistogram backend_hit_;
    LatencyHistogram full_miss_;
};

}  // namespace walde
