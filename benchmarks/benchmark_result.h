#pragma once

#include "latency_instrumentation.h"

#include <cstdint>
#include <string>

namespace walde {

struct BenchmarkConfig {
    std::string workload_type;
    uint32_t    cache_capacity;
    uint32_t    num_threads;
    uint32_t    num_ops;
    double      zipf_alpha;
    double      scan_probability;
    std::string trace_file;
};

struct BenchmarkResult {
    BenchmarkConfig config;

    double   throughput_ops_sec;
    double   duration_sec;
    uint64_t total_ops;

    double   l1_hit_rate;
    double   l2_hit_rate;
    uint64_t l1_hits;
    uint64_t l1_misses;
    uint64_t l2_hits;
    uint64_t admissions;
    uint64_t rejections;
    uint64_t evictions;

    struct PathLatency {
        double   p50;
        double   p95;
        double   p99;
        double   p999;
        double   mean;
        uint64_t count;
    };

    PathLatency total;
    PathLatency l1_hit_path;
    PathLatency l2_hit_path;
    PathLatency backend_hit_path;

    PathLatency lock_wait;
    PathLatency lookup;
    PathLatency admission;
    PathLatency eviction;
    PathLatency l2_access;
    PathLatency backend_access;
    PathLatency slab_alloc;

    static PathLatency from_histogram(const LatencyHistogram& h) {
        return {
            h.percentile_us(0.50),
            h.percentile_us(0.95),
            h.percentile_us(0.99),
            h.percentile_us(0.999),
            h.mean_us(),
            h.count()
        };
    }

    void populate_from_tracker(const PathLatencyTracker& t) {
        total            = from_histogram(t.total_);
        l1_hit_path      = from_histogram(t.l1_hit_);
        l2_hit_path      = from_histogram(t.l2_hit_);
        backend_hit_path = from_histogram(t.backend_hit_);
        lock_wait        = from_histogram(t.lock_wait_);
        lookup           = from_histogram(t.lookup_);
        admission        = from_histogram(t.admission_);
        eviction         = from_histogram(t.eviction_);
        l2_access        = from_histogram(t.l2_);
        backend_access   = from_histogram(t.backend_);
        slab_alloc       = from_histogram(t.slab_);
    }
};

}  // namespace walde
