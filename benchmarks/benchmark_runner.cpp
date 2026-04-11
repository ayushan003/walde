// ─── benchmark_runner.cpp ───────────────────────────────────
//
// Unified head-to-head comparison of three cache policies:
//   1. Baseline LRU
//   2. W-TinyLFU (with Bloom doorkeeper)
//   3. WALDE (full multi-level engine)
//
// All three are tested under identical conditions:
//   - Same operation sequence (pre-generated, shared)
//   - Same cache capacity
//   - Same working set size
//   - Same warmup phase
//   - Fixed random seed for reproducibility
//
// Workloads: YCSB A/B/C/D/F, scan resistance, uniform (failure case).
//
// Build:
//   cmake -DWALDE_BUILD_COMPARISON=ON ..
//   make walde_comparison
//   ./walde_comparison [--cache-size N] [--num-keys N]
//                      [--zipf-alpha F] [--ops N] [--threads N]
//                      [--trace FILE]

#include "lru_cache.h"
#include "wtinylfu_cache.h"
#include "bench_workloads.h"
#include "benchmark_result.h"
#include "trace_bench.h"

// WALDE headers
#include "cache_engine.h"
#include "trace_loader.h"
#include "latency_instrumentation.h"
#include "storage_backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using Clock = std::chrono::steady_clock;

// ─── Prevent dead-code elimination ──────────────────────────
template<typename T>
void do_not_optimize(const T& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

// ═════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════

struct RunConfig {
    uint32_t    cache_size   = 8192;
    uint32_t    num_keys     = 100000;
    double      zipf_alpha   = 0.99;
    uint32_t    warmup_ops   = 100000;
    uint32_t    measure_ops  = 500000;
    uint32_t    num_threads  = 1;
    uint32_t    num_runs     = 1;
    std::string trace_file;
};

// ═════════════════════════════════════════════════════════════
// Latency recording (shared across all policies)
// ═════════════════════════════════════════════════════════════

struct PolicyResult {
    std::string name;
    double      hit_rate         = 0.0;
    double      l1_hit_rate      = 0.0;
    double      l2_hit_rate      = 0.0;
    double      admission_rate   = 0.0;
    double      rejection_rate   = 0.0;
    double      throughput       = 0.0;
    double      p50_us           = 0.0;
    double      p95_us           = 0.0;
    double      p99_us           = 0.0;
    double      mean_us          = 0.0;
    uint64_t    total_ops        = 0;
    double      duration_sec     = 0.0;
    uint64_t    evictions        = 0;
    size_t      approx_memory    = 0;
};

// ═════════════════════════════════════════════════════════════
// Pre-generate workload
// ═════════════════════════════════════════════════════════════

std::vector<bench::BenchOp> generate_ops(
        const bench::BenchWorkloadConfig& cfg,
        uint32_t warmup, uint32_t measure) {
    bench::BenchWorkloadGenerator gen(cfg);
    return gen.generate_batch(warmup + measure);
}

std::vector<bench::BenchOp> generate_scan_ops(
        const bench::ScanWorkloadConfig& cfg,
        uint32_t warmup, uint32_t measure) {
    bench::ScanWorkloadGenerator gen(cfg);
    return gen.generate_batch(warmup + measure);
}

// ═════════════════════════════════════════════════════════════
// Populate backend for WALDE
// ═════════════════════════════════════════════════════════════

std::unique_ptr<walde::InMemoryBackend> make_backend(uint32_t num_keys) {
    auto backend = std::make_unique<walde::InMemoryBackend>();
    for (uint32_t i = 0; i < num_keys; ++i) {
        backend->put("key_" + std::to_string(i),
                     "value_" + std::to_string(i));
    }
    return backend;
}

// ─── Backend populated from trace keys ──────────────────────
//
// For trace-driven benchmarks, the backend must contain the
// trace's actual keys — not synthetic key_0..key_N — otherwise
// WALDE's backend lookup returns nothing and no items ever
// enter the cache.

std::unique_ptr<walde::InMemoryBackend> make_trace_backend(
        const std::unordered_set<std::string>& keys) {
    auto backend = std::make_unique<walde::InMemoryBackend>();
    for (const auto& key : keys) {
        backend->put(key, "trace_val_" + key);
    }
    return backend;
}

// ─── Run WALDE with a pre-built backend ─────────────────────
//
// Same as run_walde but accepts an externally built backend.
// Used by trace benchmarks so that trace keys exist in the
// backend. Does NOT modify the original run_walde function.

PolicyResult run_walde_with_backend(
        const std::vector<bench::BenchOp>& ops,
        uint32_t warmup, uint32_t capacity,
        std::unique_ptr<walde::InMemoryBackend> backend,
        walde::PathLatencyTracker* out_tracker = nullptr) {
    walde::CacheEngine::Config cfg;
    cfg.l1_config.total_capacity = capacity;
    cfg.l1_config.window_pct     = 0.01f;
    cfg.l1_config.probation_pct  = 0.20f;
    cfg.l2_capacity              = capacity / 2;
    cfg.demotion_queue_size      = 2048;

    auto engine = std::make_unique<walde::CacheEngine>(cfg, std::move(backend));

    for (uint32_t i = 0; i < warmup && i < ops.size(); ++i) {
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT || op.type == bench::BenchOp::RMW)
            engine->put(op.key, op.value);
        else {
            auto v = engine->get(op.key);
            do_not_optimize(v);
        }
    }

    uint64_t h1_before  = engine->l1_hits();
    uint64_t m1_before  = engine->l1_misses();
    uint64_t h2_before  = engine->l2_hits();
    uint64_t adm_before = engine->l1_admissions();
    uint64_t rej_before = engine->l1_rejections();
    uint64_t ev_before  = engine->l1_evictions();

    walde::PathLatencyTracker tracker;
    uint32_t start = warmup;
    uint32_t end   = static_cast<uint32_t>(ops.size());
    auto wall_start = Clock::now();

    for (uint32_t i = start; i < end; ++i) {
        walde::LatencyBreakdown bd;
        auto t0  = Clock::now();
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT) {
            engine->put(op.key, op.value, &bd);
        } else if (op.type == bench::BenchOp::RMW) {
            auto v = engine->get(op.key, &bd);
            do_not_optimize(v);
            engine->put(op.key, op.value);
        } else {
            auto v = engine->get(op.key, &bd);
            do_not_optimize(v);
        }
        auto t1 = Clock::now();
        bd.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        tracker.record(bd);
    }

    auto wall_end = Clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    uint64_t mops = end - start;

    uint64_t bench_h1  = engine->l1_hits()      - h1_before;
    uint64_t bench_m1  = engine->l1_misses()     - m1_before;
    uint64_t bench_h2  = engine->l2_hits()       - h2_before;
    uint64_t bench_adm = engine->l1_admissions() - adm_before;
    uint64_t bench_rej = engine->l1_rejections() - rej_before;
    uint64_t bench_ev  = engine->l1_evictions()  - ev_before;

    double l1_hr   = (bench_h1 + bench_m1 > 0) ? static_cast<double>(bench_h1) / (bench_h1 + bench_m1) : 0.0;
    double l2_hr   = (bench_m1 > 0)             ? static_cast<double>(bench_h2) / bench_m1              : 0.0;
    double overall = (bench_h1 + bench_m1 > 0) ? static_cast<double>(bench_h1 + bench_h2) / (bench_h1 + bench_m1) : 0.0;

    PolicyResult r;
    r.name           = "WALDE";
    r.hit_rate       = overall;
    r.l1_hit_rate    = l1_hr;
    r.l2_hit_rate    = l2_hr;
    r.admission_rate = (bench_adm + bench_rej > 0) ? static_cast<double>(bench_adm) / (bench_adm + bench_rej) : 0.0;
    r.rejection_rate = (bench_adm + bench_rej > 0) ? static_cast<double>(bench_rej) / (bench_adm + bench_rej) : 0.0;
    r.throughput     = mops / wall_sec;
    r.p50_us         = tracker.total_.percentile_us(0.50);
    r.p95_us         = tracker.total_.percentile_us(0.95);
    r.p99_us         = tracker.total_.percentile_us(0.99);
    r.mean_us        = tracker.total_.mean_us();
    r.total_ops      = mops;
    r.duration_sec   = wall_sec;
    r.evictions      = bench_ev;
    r.approx_memory  = static_cast<size_t>(capacity + 64) * 128
                     + 4 * 8192 * 4
                     + static_cast<size_t>(capacity / 2) * (10 + 10 + 48 + 64)
                     + static_cast<size_t>(capacity) * 64;

    if (out_tracker) *out_tracker = tracker;
    return r;
}

// ═════════════════════════════════════════════════════════════
// Run LRU
// ═════════════════════════════════════════════════════════════

PolicyResult run_lru(const std::vector<bench::BenchOp>& ops,
                     uint32_t warmup, uint32_t capacity) {
    bench::LRUCache cache(capacity);

    for (uint32_t i = 0; i < warmup && i < ops.size(); ++i) {
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT || op.type == bench::BenchOp::RMW) {
            cache.put(op.key, op.value);
        } else {
            auto v = cache.get(op.key);
            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
            do_not_optimize(v);
        }
    }
    cache.reset_stats();

    walde::LatencyHistogram hist;
    uint32_t start = warmup;
    uint32_t end   = static_cast<uint32_t>(ops.size());
    auto wall_start = Clock::now();

    for (uint32_t i = start; i < end; ++i) {
        auto t0 = Clock::now();
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT) {
            cache.put(op.key, op.value);
        } else if (op.type == bench::BenchOp::RMW) {
            auto v = cache.get(op.key);
            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
            do_not_optimize(v);
            cache.put(op.key, op.value);
        } else {
            auto v = cache.get(op.key);
            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
            do_not_optimize(v);
        }
        auto t1 = Clock::now();
        hist.record(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    auto wall_end = Clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    uint64_t mops = end - start;

    PolicyResult r;
    r.name         = "LRU";
    r.hit_rate     = cache.hit_rate();
    r.l1_hit_rate  = r.hit_rate;
    r.throughput   = mops / wall_sec;
    r.p50_us       = hist.percentile_us(0.50);
    r.p95_us       = hist.percentile_us(0.95);
    r.p99_us       = hist.percentile_us(0.99);
    r.mean_us      = hist.mean_us();
    r.total_ops    = mops;
    r.duration_sec = wall_sec;
    r.evictions    = cache.evictions();
    r.approx_memory = static_cast<size_t>(capacity) * (10 + 10 + 48 + 64);
    return r;
}

// ═════════════════════════════════════════════════════════════
// Run W-TinyLFU
// ═════════════════════════════════════════════════════════════

PolicyResult run_wtinylfu(const std::vector<bench::BenchOp>& ops,
                          uint32_t warmup, uint32_t capacity) {
    bench::WTinyLFUCache cache(capacity, 0.01f, 0.20f);

    for (uint32_t i = 0; i < warmup && i < ops.size(); ++i) {
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT || op.type == bench::BenchOp::RMW) {
            cache.put(op.key, op.value);
        } else {
            auto v = cache.get(op.key);
            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
            do_not_optimize(v);
        }
    }
    cache.reset_stats();

    walde::LatencyHistogram hist;
    uint32_t start = warmup;
    uint32_t end   = static_cast<uint32_t>(ops.size());
    auto wall_start = Clock::now();

    for (uint32_t i = start; i < end; ++i) {
        auto t0 = Clock::now();
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT) {
            cache.put(op.key, op.value);
        } else if (op.type == bench::BenchOp::RMW) {
            auto v = cache.get(op.key);
            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
            do_not_optimize(v);
            cache.put(op.key, op.value);
        } else {
            auto v = cache.get(op.key);
            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
            do_not_optimize(v);
        }
        auto t1 = Clock::now();
        hist.record(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    auto wall_end = Clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    uint64_t mops = end - start;
    uint64_t adm  = cache.admissions();
    uint64_t rej  = cache.rejections();

    PolicyResult r;
    r.name           = "W-TinyLFU";
    r.hit_rate       = cache.hit_rate();
    r.l1_hit_rate    = r.hit_rate;
    r.admission_rate = (adm + rej > 0) ? static_cast<double>(adm) / (adm + rej) : 0.0;
    r.rejection_rate = (adm + rej > 0) ? static_cast<double>(rej) / (adm + rej) : 0.0;
    r.throughput     = mops / wall_sec;
    r.p50_us         = hist.percentile_us(0.50);
    r.p95_us         = hist.percentile_us(0.95);
    r.p99_us         = hist.percentile_us(0.99);
    r.mean_us        = hist.mean_us();
    r.total_ops      = mops;
    r.duration_sec   = wall_sec;
    r.evictions      = cache.evictions();
    r.approx_memory  = 4 * 8192 * 4 + capacity + static_cast<size_t>(capacity) * (10 + 10 + 48 + 64);
    return r;
}

// ═════════════════════════════════════════════════════════════
// Run WALDE
// ═════════════════════════════════════════════════════════════

PolicyResult run_walde(const std::vector<bench::BenchOp>& ops,
                       uint32_t warmup, uint32_t capacity,
                       uint32_t num_keys,
                       walde::PathLatencyTracker* out_tracker = nullptr) {
    walde::CacheEngine::Config cfg;
    cfg.l1_config.total_capacity = capacity;
    cfg.l1_config.window_pct     = 0.01f;
    cfg.l1_config.probation_pct  = 0.20f;
    cfg.l2_capacity              = capacity / 2;
    cfg.demotion_queue_size      = 2048;

    auto backend = make_backend(num_keys);
    auto engine  = std::make_unique<walde::CacheEngine>(cfg, std::move(backend));

    for (uint32_t i = 0; i < warmup && i < ops.size(); ++i) {
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT || op.type == bench::BenchOp::RMW)
            engine->put(op.key, op.value);
        else {
            auto v = engine->get(op.key);
            do_not_optimize(v);
        }
    }

    uint64_t h1_before  = engine->l1_hits();
    uint64_t m1_before  = engine->l1_misses();
    uint64_t h2_before  = engine->l2_hits();
    uint64_t adm_before = engine->l1_admissions();
    uint64_t rej_before = engine->l1_rejections();
    uint64_t ev_before  = engine->l1_evictions();

    walde::PathLatencyTracker tracker;
    uint32_t start = warmup;
    uint32_t end   = static_cast<uint32_t>(ops.size());
    auto wall_start = Clock::now();

    for (uint32_t i = start; i < end; ++i) {
        walde::LatencyBreakdown bd;
        auto t0  = Clock::now();
        auto& op = ops[i];
        if (op.type == bench::BenchOp::PUT) {
            engine->put(op.key, op.value, &bd);
        } else if (op.type == bench::BenchOp::RMW) {
            auto v = engine->get(op.key, &bd);
            do_not_optimize(v);
            engine->put(op.key, op.value);
        } else {
            auto v = engine->get(op.key, &bd);
            do_not_optimize(v);
        }
        auto t1 = Clock::now();
        bd.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        tracker.record(bd);
    }

    auto wall_end = Clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    uint64_t mops = end - start;

    uint64_t bench_h1  = engine->l1_hits()      - h1_before;
    uint64_t bench_m1  = engine->l1_misses()     - m1_before;
    uint64_t bench_h2  = engine->l2_hits()       - h2_before;
    uint64_t bench_adm = engine->l1_admissions() - adm_before;
    uint64_t bench_rej = engine->l1_rejections() - rej_before;
    uint64_t bench_ev  = engine->l1_evictions()  - ev_before;

    double l1_hr  = (bench_h1 + bench_m1 > 0) ? static_cast<double>(bench_h1) / (bench_h1 + bench_m1) : 0.0;
    double l2_hr  = (bench_m1 > 0)             ? static_cast<double>(bench_h2) / bench_m1              : 0.0;
    double overall= (bench_h1 + bench_m1 > 0) ? static_cast<double>(bench_h1 + bench_h2) / (bench_h1 + bench_m1) : 0.0;

    PolicyResult r;
    r.name           = "WALDE";
    r.hit_rate       = overall;
    r.l1_hit_rate    = l1_hr;
    r.l2_hit_rate    = l2_hr;
    r.admission_rate = (bench_adm + bench_rej > 0) ? static_cast<double>(bench_adm) / (bench_adm + bench_rej) : 0.0;
    r.rejection_rate = (bench_adm + bench_rej > 0) ? static_cast<double>(bench_rej) / (bench_adm + bench_rej) : 0.0;
    r.throughput     = mops / wall_sec;
    r.p50_us         = tracker.total_.percentile_us(0.50);
    r.p95_us         = tracker.total_.percentile_us(0.95);
    r.p99_us         = tracker.total_.percentile_us(0.99);
    r.mean_us        = tracker.total_.mean_us();
    r.total_ops      = mops;
    r.duration_sec   = wall_sec;
    r.evictions      = bench_ev;
    r.approx_memory  = static_cast<size_t>(capacity + 64) * 128
                     + 4 * 8192 * 4
                     + static_cast<size_t>(capacity / 2) * (10 + 10 + 48 + 64)
                     + static_cast<size_t>(capacity) * 64;

    if (out_tracker) *out_tracker = tracker;
    return r;
}

// ═════════════════════════════════════════════════════════════
// Pretty printer — fixed-width, clean box-drawing
//
// Box width: 78 chars between the two │ characters (80 total with borders).
// Column layout for policy rows:
//   ║ %-11s  %8s  %11s  %8s  %8s  %8s  %8s ║
//   name(11) hr(8) tput(11) p50(8) p95(8) p99(8) mem(8)
// ═════════════════════════════════════════════════════════════

// Horizontal rules — exactly 78 chars between ╔/╠/╚ and ╗/╣/╝
#define BOX_TOP    "╔══════════════════════════════════════════════════════════════════════════════╗"
#define BOX_SEP    "╠══════════════════════════════════════════════════════════════════════════════╣"
#define BOX_BOT    "╚══════════════════════════════════════════════════════════════════════════════╝"
// Width of content between ║ and ║: 78 chars
#define BOX_WIDTH  78

static void box_line(const char* content) {
    // Print ║ + content padded/truncated to BOX_WIDTH + ║
    std::printf("║ %-*s ║\n", BOX_WIDTH - 2, content);
}

void print_header(const char* workload_name, double zipf_alpha,
                  uint32_t cache_size, uint32_t num_keys) {
    char buf[256];
    std::printf("\n%s\n", BOX_TOP);

    std::snprintf(buf, sizeof(buf), "Workload : %s", workload_name);
    box_line(buf);

    std::snprintf(buf, sizeof(buf),
        "Zipf α=%.2f  |  Cache=%u  |  Working set=%u  |  Ratio=%.1f%%",
        zipf_alpha, cache_size, num_keys, 100.0 * cache_size / num_keys);
    box_line(buf);

    std::printf("%s\n", BOX_SEP);

    // Column header — fixed widths matching data rows
    std::printf("║ %-11s  %8s  %11s  %8s  %8s  %8s  %7s ║\n",
                "Policy", "Hit Rate", "Throughput", "p50(us)", "p95(us)", "p99(us)", "Mem(KB)");

    std::printf("%s\n", BOX_SEP);
}

void print_row(const PolicyResult& r) {
    char tput_buf[32];
    if (r.throughput >= 1e6)
        std::snprintf(tput_buf, sizeof(tput_buf), "%.2fM/s", r.throughput / 1e6);
    else if (r.throughput >= 1e3)
        std::snprintf(tput_buf, sizeof(tput_buf), "%.0fK/s", r.throughput / 1e3);
    else
        std::snprintf(tput_buf, sizeof(tput_buf), "%.0f/s", r.throughput);

    std::printf("║ %-11s  %7.1f%%  %11s  %8.2f  %8.2f  %8.2f  %7zu ║\n",
                r.name.c_str(),
                r.hit_rate * 100.0,
                tput_buf,
                r.p50_us, r.p95_us, r.p99_us,
                r.approx_memory / 1024);
}

void print_footer() {
    std::printf("%s\n", BOX_BOT);
}

void print_admission_details(const PolicyResult& r) {
    if (r.admission_rate > 0 || r.rejection_rate > 0) {
        std::printf("    %-12s admit=%5.1f%%  reject=%5.1f%%  evictions=%lu\n",
                    (r.name + ":").c_str(),
                    r.admission_rate * 100.0,
                    r.rejection_rate * 100.0,
                    static_cast<unsigned long>(r.evictions));
    }
}

void print_walde_breakdown(const PolicyResult& r) {
    if (r.name == "WALDE") {
        std::printf("    WALDE detail : L1 hit=%5.1f%%  L2 hit=%5.1f%% (of L1 misses)\n",
                    r.l1_hit_rate * 100.0, r.l2_hit_rate * 100.0);
    }
}

void print_walde_latency_breakdown(const walde::PathLatencyTracker& t) {
    std::printf("    WALDE latency breakdown (p50 μs):\n");
    std::printf("      %-12s %6.3f  |  %-12s %6.3f  |  %-12s %6.3f\n",
                "Lock wait:", t.lock_wait_.percentile_us(0.50),
                "Lookup:",    t.lookup_.percentile_us(0.50),
                "Admission:", t.admission_.percentile_us(0.50));
    std::printf("      %-12s %6.3f  |  %-12s %6.3f  |  %-12s %6.3f  |  %-12s %6.3f\n",
                "Eviction:",  t.eviction_.percentile_us(0.50),
                "Slab:",      t.slab_.percentile_us(0.50),
                "L2:",        t.l2_.percentile_us(0.50),
                "Backend:",   t.backend_.percentile_us(0.50));
    std::printf("    Per-path end-to-end (p50 μs):\n");
    std::printf("      L1 hit: %.3f (%lu ops)  "
                "L2 hit: %.3f (%lu ops)  "
                "Backend: %.3f (%lu ops)\n",
                t.l1_hit_.percentile_us(0.50),    t.l1_hit_.count(),
                t.l2_hit_.percentile_us(0.50),    t.l2_hit_.count(),
                t.backend_hit_.percentile_us(0.50), t.backend_hit_.count());
}

// ═════════════════════════════════════════════════════════════
// Run one workload across all three policies
// ═════════════════════════════════════════════════════════════

using OpsGenFn = std::function<std::vector<bench::BenchOp>(uint64_t seed)>;

void run_workload(const char* name,
                  const std::vector<bench::BenchOp>& ops,
                  const RunConfig& rc,
                  OpsGenFn ops_gen = nullptr) {
    print_header(name, rc.zipf_alpha, rc.cache_size, rc.num_keys);

    auto r_lru = run_lru(ops, rc.warmup_ops, rc.cache_size);
    print_row(r_lru);

    auto r_wtlfu = run_wtinylfu(ops, rc.warmup_ops, rc.cache_size);
    print_row(r_wtlfu);

    walde::PathLatencyTracker walde_tracker;
    auto r_walde = run_walde(ops, rc.warmup_ops, rc.cache_size,
                             rc.num_keys, &walde_tracker);
    print_row(r_walde);
    print_footer();

    std::printf("\n  Admission details:\n");
    print_admission_details(r_wtlfu);
    print_admission_details(r_walde);
    print_walde_breakdown(r_walde);
    std::printf("\n");
    print_walde_latency_breakdown(walde_tracker);

    if (rc.num_runs > 1 && ops_gen) {
        std::printf("\n  ── Variance over %u runs (varying seed) ──\n", rc.num_runs);

        auto run_variance = [&](const char* policy_name, auto run_fn) {
            std::vector<double> tputs, hit_rates;
            for (uint32_t r = 0; r < rc.num_runs; ++r) {
                uint64_t run_seed = 42 + static_cast<uint64_t>(r) * 7919;
                auto run_ops = ops_gen(run_seed);
                auto result  = run_fn(run_ops);
                tputs.push_back(result.throughput);
                hit_rates.push_back(result.hit_rate);
            }
            double tput_mean = std::accumulate(tputs.begin(), tputs.end(), 0.0) / tputs.size();
            double hr_mean   = std::accumulate(hit_rates.begin(), hit_rates.end(), 0.0) / hit_rates.size();
            double tput_var  = 0.0, hr_var = 0.0;
            for (size_t i = 0; i < tputs.size(); ++i) {
                tput_var += (tputs[i] - tput_mean) * (tputs[i] - tput_mean);
                hr_var   += (hit_rates[i] - hr_mean) * (hit_rates[i] - hr_mean);
            }
            double tput_std = std::sqrt(tput_var / tputs.size());
            double hr_std   = std::sqrt(hr_var   / hit_rates.size());
            std::printf("    %-12s  throughput: %8.0f ± %6.0f ops/s  "
                        "hit_rate: %.2f%% ± %.2f%%\n",
                        policy_name, tput_mean, tput_std,
                        hr_mean * 100.0, hr_std * 100.0);
        };

        run_variance("LRU", [&](const std::vector<bench::BenchOp>& run_ops) {
            return run_lru(run_ops, rc.warmup_ops, rc.cache_size);
        });
        run_variance("W-TinyLFU", [&](const std::vector<bench::BenchOp>& run_ops) {
            return run_wtinylfu(run_ops, rc.warmup_ops, rc.cache_size);
        });
        run_variance("WALDE", [&](const std::vector<bench::BenchOp>& run_ops) {
            return run_walde(run_ops, rc.warmup_ops, rc.cache_size, rc.num_keys);
        });
    }
    std::printf("\n");
}

// ═════════════════════════════════════════════════════════════
// Multi-threaded concurrency scaling benchmark
//
// Box width matches the main table (80 chars total).
// Column layout:
//   ║ %7s  %-11s  %12s  %10s  %11s ║
//   threads(7) policy(11) tput(12) hit_rate(10) p99(11)
// ═════════════════════════════════════════════════════════════

void run_concurrency_scaling(const RunConfig& rc) {
    std::printf("\n%s\n", BOX_TOP);
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "Concurrency Scaling  |  YCSB-B, Zipf α=%.2f, cache=%u",
            rc.zipf_alpha, rc.cache_size);
        box_line(buf);
    }
    std::printf("%s\n", BOX_SEP);
    std::printf("║ %7s  %-11s  %12s  %10s  %11s ║\n",
                "Threads", "Policy", "Throughput", "Hit Rate", "p99 (us)");
    std::printf("%s\n", BOX_SEP);

    bool first_block = true;
    for (int threads : {1, 2, 4, 8}) {
        if (!first_block) {
            std::printf("║%78s║\n", "");  // blank separator row
        }
        first_block = false;

        uint32_t ops_per_thread = rc.measure_ops / threads;

        // Helper lambda: warmup + threaded measure for a generic cache
        // Each policy section is self-contained below.

        // --- LRU ---
        {
            bench::LRUCache cache(rc.cache_size);
            bench::BenchWorkloadConfig wcfg;
            wcfg.workload   = bench::YCSBWorkload::B;
            wcfg.num_keys   = rc.num_keys;
            wcfg.zipf_alpha = rc.zipf_alpha;
            wcfg.seed       = 42;

            bench::BenchWorkloadGenerator warmup_gen(wcfg);
            for (uint32_t i = 0; i < rc.warmup_ops; ++i) {
                auto op = warmup_gen.next();
                if (op.type == bench::BenchOp::PUT)
                    cache.put(op.key, op.value);
                else {
                    auto v = cache.get(op.key);
                    if (!v.has_value()) cache.put(op.key, "value_" + op.key);
                    do_not_optimize(v);
                }
            }
            cache.reset_stats();

            std::vector<walde::LatencyHistogram> per_thread(threads);
            auto wall_start = Clock::now();
            std::vector<std::thread> tvec;
            for (int t = 0; t < threads; ++t) {
                tvec.emplace_back([&, t]() {
                    bench::BenchWorkloadConfig tc = wcfg;
                    tc.seed = 42 + t * 7919;
                    bench::BenchWorkloadGenerator gen(tc);
                    for (uint32_t i = 0; i < ops_per_thread; ++i) {
                        auto op = gen.next();
                        auto t0 = Clock::now();
                        if (op.type == bench::BenchOp::PUT)
                            cache.put(op.key, op.value);
                        else {
                            auto v = cache.get(op.key);
                            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
                            do_not_optimize(v);
                        }
                        per_thread[t].record(std::chrono::duration_cast<
                            std::chrono::nanoseconds>(Clock::now() - t0).count());
                    }
                });
            }
            for (auto& t : tvec) t.join();
            double wall_sec = std::chrono::duration<double>(Clock::now() - wall_start).count();
            uint64_t total  = static_cast<uint64_t>(ops_per_thread) * threads;
            walde::LatencyHistogram merged;
            for (auto& h : per_thread) merged.merge(h);

            char tput[24];
            double tp = total / wall_sec;
            if (tp >= 1e6) std::snprintf(tput, sizeof(tput), "%.2fM/s", tp / 1e6);
            else           std::snprintf(tput, sizeof(tput), "%.0fK/s", tp / 1e3);

            std::printf("║ %7d  %-11s  %12s  %9.1f%%  %10.2f us ║\n",
                        threads, "LRU", tput,
                        cache.hit_rate() * 100.0,
                        merged.percentile_us(0.99));
        }

        // --- StripedLRU ---
        {
            bench::StripedLRUCache cache(rc.cache_size);
            bench::BenchWorkloadConfig wcfg;
            wcfg.workload   = bench::YCSBWorkload::B;
            wcfg.num_keys   = rc.num_keys;
            wcfg.zipf_alpha = rc.zipf_alpha;
            wcfg.seed       = 42;

            bench::BenchWorkloadGenerator warmup_gen(wcfg);
            for (uint32_t i = 0; i < rc.warmup_ops; ++i) {
                auto op = warmup_gen.next();
                if (op.type == bench::BenchOp::PUT)
                    cache.put(op.key, op.value);
                else {
                    auto v = cache.get(op.key);
                    if (!v.has_value()) cache.put(op.key, "value_" + op.key);
                    do_not_optimize(v);
                }
            }
            cache.reset_stats();

            std::vector<walde::LatencyHistogram> per_thread(threads);
            auto wall_start = Clock::now();
            std::vector<std::thread> tvec;
            for (int t = 0; t < threads; ++t) {
                tvec.emplace_back([&, t]() {
                    bench::BenchWorkloadConfig tc = wcfg;
                    tc.seed = 42 + t * 7919;
                    bench::BenchWorkloadGenerator gen(tc);
                    for (uint32_t i = 0; i < ops_per_thread; ++i) {
                        auto op = gen.next();
                        auto t0 = Clock::now();
                        if (op.type == bench::BenchOp::PUT)
                            cache.put(op.key, op.value);
                        else {
                            auto v = cache.get(op.key);
                            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
                            do_not_optimize(v);
                        }
                        per_thread[t].record(std::chrono::duration_cast<
                            std::chrono::nanoseconds>(Clock::now() - t0).count());
                    }
                });
            }
            for (auto& t : tvec) t.join();
            double wall_sec = std::chrono::duration<double>(Clock::now() - wall_start).count();
            uint64_t total  = static_cast<uint64_t>(ops_per_thread) * threads;
            walde::LatencyHistogram merged;
            for (auto& h : per_thread) merged.merge(h);

            char tput[24];
            double tp = total / wall_sec;
            if (tp >= 1e6) std::snprintf(tput, sizeof(tput), "%.2fM/s", tp / 1e6);
            else           std::snprintf(tput, sizeof(tput), "%.0fK/s", tp / 1e3);

            std::printf("║ %7d  %-11s  %12s  %9.1f%%  %10.2f us ║\n",
                        threads, "StripedLRU", tput,
                        cache.hit_rate() * 100.0,
                        merged.percentile_us(0.99));
        }

        // --- W-TinyLFU ---
        {
            bench::WTinyLFUCache cache(rc.cache_size, 0.01f, 0.20f);
            bench::BenchWorkloadConfig wcfg;
            wcfg.workload   = bench::YCSBWorkload::B;
            wcfg.num_keys   = rc.num_keys;
            wcfg.zipf_alpha = rc.zipf_alpha;
            wcfg.seed       = 42;

            bench::BenchWorkloadGenerator warmup_gen(wcfg);
            for (uint32_t i = 0; i < rc.warmup_ops; ++i) {
                auto op = warmup_gen.next();
                if (op.type == bench::BenchOp::PUT)
                    cache.put(op.key, op.value);
                else {
                    auto v = cache.get(op.key);
                    if (!v.has_value()) cache.put(op.key, "value_" + op.key);
                    do_not_optimize(v);
                }
            }
            cache.reset_stats();

            std::vector<walde::LatencyHistogram> per_thread(threads);
            auto wall_start = Clock::now();
            std::vector<std::thread> tvec;
            for (int t = 0; t < threads; ++t) {
                tvec.emplace_back([&, t]() {
                    bench::BenchWorkloadConfig tc = wcfg;
                    tc.seed = 42 + t * 7919;
                    bench::BenchWorkloadGenerator gen(tc);
                    for (uint32_t i = 0; i < ops_per_thread; ++i) {
                        auto op = gen.next();
                        auto t0 = Clock::now();
                        if (op.type == bench::BenchOp::PUT)
                            cache.put(op.key, op.value);
                        else {
                            auto v = cache.get(op.key);
                            if (!v.has_value()) cache.put(op.key, "value_" + op.key);
                            do_not_optimize(v);
                        }
                        per_thread[t].record(std::chrono::duration_cast<
                            std::chrono::nanoseconds>(Clock::now() - t0).count());
                    }
                });
            }
            for (auto& t : tvec) t.join();
            double wall_sec = std::chrono::duration<double>(Clock::now() - wall_start).count();
            uint64_t total  = static_cast<uint64_t>(ops_per_thread) * threads;
            walde::LatencyHistogram merged;
            for (auto& h : per_thread) merged.merge(h);

            char tput[24];
            double tp = total / wall_sec;
            if (tp >= 1e6) std::snprintf(tput, sizeof(tput), "%.2fM/s", tp / 1e6);
            else           std::snprintf(tput, sizeof(tput), "%.0fK/s", tp / 1e3);

            std::printf("║ %7d  %-11s  %12s  %9.1f%%  %10.2f us ║\n",
                        threads, "W-TinyLFU", tput,
                        cache.hit_rate() * 100.0,
                        merged.percentile_us(0.99));
        }

        // --- WALDE ---
        {
            walde::CacheEngine::Config ecfg;
            ecfg.l1_config.total_capacity = rc.cache_size;
            ecfg.l2_capacity              = rc.cache_size / 2;
            ecfg.demotion_queue_size      = 2048;

            auto backend = make_backend(rc.num_keys);
            auto engine  = std::make_unique<walde::CacheEngine>(ecfg, std::move(backend));

            bench::BenchWorkloadConfig wcfg;
            wcfg.workload   = bench::YCSBWorkload::B;
            wcfg.num_keys   = rc.num_keys;
            wcfg.zipf_alpha = rc.zipf_alpha;
            wcfg.seed       = 42;

            bench::BenchWorkloadGenerator warmup_gen(wcfg);
            for (uint32_t i = 0; i < rc.warmup_ops; ++i) {
                auto op = warmup_gen.next();
                if (op.type == bench::BenchOp::PUT)
                    engine->put(op.key, op.value);
                else {
                    auto v = engine->get(op.key);
                    do_not_optimize(v);
                }
            }

            std::vector<walde::LatencyHistogram> per_thread(threads);
            auto wall_start = Clock::now();
            std::vector<std::thread> tvec;
            for (int t = 0; t < threads; ++t) {
                tvec.emplace_back([&, t]() {
                    bench::BenchWorkloadConfig tc = wcfg;
                    tc.seed = 42 + t * 7919;
                    bench::BenchWorkloadGenerator gen(tc);
                    for (uint32_t i = 0; i < ops_per_thread; ++i) {
                        auto op = gen.next();
                        auto t0 = Clock::now();
                        if (op.type == bench::BenchOp::PUT)
                            engine->put(op.key, op.value);
                        else {
                            auto v = engine->get(op.key);
                            do_not_optimize(v);
                        }
                        per_thread[t].record(std::chrono::duration_cast<
                            std::chrono::nanoseconds>(Clock::now() - t0).count());
                    }
                });
            }
            for (auto& t : tvec) t.join();
            double wall_sec = std::chrono::duration<double>(Clock::now() - wall_start).count();
            uint64_t total  = static_cast<uint64_t>(ops_per_thread) * threads;
            walde::LatencyHistogram merged;
            for (auto& h : per_thread) merged.merge(h);

            char tput[24];
            double tp = total / wall_sec;
            if (tp >= 1e6) std::snprintf(tput, sizeof(tput), "%.2fM/s", tp / 1e6);
            else           std::snprintf(tput, sizeof(tput), "%.0fK/s", tp / 1e3);

            std::printf("║ %7d  %-11s  %12s  %9.1f%%  %10.2f us ║\n",
                        threads, "WALDE", tput,
                        engine->overall_hit_rate() * 100.0,
                        merged.percentile_us(0.99));
        }
    }

    std::printf("%s\n\n", BOX_BOT);
}

// ═════════════════════════════════════════════════════════════
// CLI parser
// ═════════════════════════════════════════════════════════════

RunConfig parse_args(int argc, char* argv[]) {
    RunConfig rc;
    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--cache-size")  == 0 && i+1 < argc) rc.cache_size  = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--num-keys")    == 0 && i+1 < argc) rc.num_keys    = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--zipf-alpha")  == 0 && i+1 < argc) rc.zipf_alpha  = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--ops")         == 0 && i+1 < argc) rc.measure_ops = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--warmup")      == 0 && i+1 < argc) rc.warmup_ops  = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--threads")     == 0 && i+1 < argc) rc.num_threads = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--runs")        == 0 && i+1 < argc) rc.num_runs    = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--trace")       == 0 && i+1 < argc) rc.trace_file  = argv[++i];
        else if (std::strcmp(argv[i], "--help")        == 0) {
            std::printf("Usage: %s [OPTIONS]\n", argv[0]);
            std::printf("  --cache-size N   L1 cache capacity (default: 8192)\n");
            std::printf("  --num-keys N     Working set size (default: 100000)\n");
            std::printf("  --zipf-alpha F   Zipfian skew (default: 0.99)\n");
            std::printf("  --ops N          Measurement ops (default: 500000)\n");
            std::printf("  --warmup N       Warmup ops (default: 100000)\n");
            std::printf("  --threads N      Max thread count for scaling (default: 1)\n");
            std::printf("  --runs N         Runs per workload for variance (default: 1)\n");
            std::printf("  --trace FILE     Trace file (one key per line)\n");
            std::exit(0);
        }
    }
    return rc;
}

// ═════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    RunConfig rc = parse_args(argc, argv);

    // ── Banner ───────────────────────────────────────────────
    std::printf("╔══════════════════════════════════════════╗\n");
    std::printf("║   WALDE Comparison Benchmark Suite       ║\n");
    std::printf("║   LRU vs W-TinyLFU vs WALDE              ║\n");
    std::printf("╠══════════════════════════════════════════╣\n");
    std::printf("║  Cache size   : %-10u               ║\n", rc.cache_size);
    std::printf("║  Working set  : %-10u               ║\n", rc.num_keys);
    std::printf("║  Zipf alpha   : %-10.2f               ║\n", rc.zipf_alpha);
    std::printf("║  Warmup ops   : %-10u               ║\n", rc.warmup_ops);
    std::printf("║  Measure ops  : %-10u               ║\n", rc.measure_ops);
    if (rc.num_runs > 1)
        std::printf("║  Seed         : 42 + run×7919 (varying) ║\n");
    else
        std::printf("║  Seed         : 42 (fixed)               ║\n");
    std::printf("║  Runs         : %-10u               ║\n", rc.num_runs);
    std::printf("╚══════════════════════════════════════════╝\n\n");

    // ── YCSB-A ───────────────────────────────────────────────
    {
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::A; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);
        run_workload("YCSB-A  (50r / 50w)", ops, rc, [=](uint64_t s){
            auto c = wcfg; c.seed = s; return generate_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── YCSB-B ───────────────────────────────────────────────
    {
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::B; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);
        run_workload("YCSB-B  (95r / 5w)", ops, rc, [=](uint64_t s){
            auto c = wcfg; c.seed = s; return generate_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── YCSB-C ───────────────────────────────────────────────
    {
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::C; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);
        run_workload("YCSB-C  (100r)", ops, rc, [=](uint64_t s){
            auto c = wcfg; c.seed = s; return generate_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── YCSB-D ───────────────────────────────────────────────
    {
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::D; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);
        run_workload("YCSB-D  (read-latest)", ops, rc, [=](uint64_t s){
            auto c = wcfg; c.seed = s; return generate_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── YCSB-F ───────────────────────────────────────────────
    {
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::F; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);
        run_workload("YCSB-F  (RMW)", ops, rc, [=](uint64_t s){
            auto c = wcfg; c.seed = s; return generate_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── Scan resistance ──────────────────────────────────────
    {
        bench::ScanWorkloadConfig scfg;
        scfg.num_keys = rc.num_keys; scfg.zipf_alpha = rc.zipf_alpha;
        scfg.scan_fraction = 0.10; scfg.scan_length = 500;
        scfg.write_ratio = 0.05; scfg.seed = 42;
        auto ops = generate_scan_ops(scfg, rc.warmup_ops, rc.measure_ops);
        run_workload("Scan Resistance  (10% scans)", ops, rc, [=](uint64_t s){
            auto c = scfg; c.seed = s; return generate_scan_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── Uniform (failure case) ───────────────────────────────
    {
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::C; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.use_uniform = true; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);
        run_workload("Uniform  (no skew — expected failure case)", ops, rc, [=](uint64_t s){
            auto c = wcfg; c.seed = s; return generate_ops(c, rc.warmup_ops, rc.measure_ops); });
    }

    // ── Fairness check: equal total capacity ─────────────────
    {
        uint32_t walde_total = rc.cache_size + rc.cache_size / 2;
        bench::BenchWorkloadConfig wcfg;
        wcfg.workload = bench::YCSBWorkload::B; wcfg.num_keys = rc.num_keys;
        wcfg.zipf_alpha = rc.zipf_alpha; wcfg.seed = 42;
        auto ops = generate_ops(wcfg, rc.warmup_ops, rc.measure_ops);

        char label[128];
        std::snprintf(label, sizeof(label),
            "Fairness check  |  Equal capacity (%u total)  |  YCSB-B", walde_total);
        print_header(label, rc.zipf_alpha, walde_total, rc.num_keys);

        auto r_lru   = run_lru(ops, rc.warmup_ops, walde_total);   print_row(r_lru);
        auto r_wtlfu = run_wtinylfu(ops, rc.warmup_ops, walde_total); print_row(r_wtlfu);
        walde::PathLatencyTracker wt;
        auto r_walde = run_walde(ops, rc.warmup_ops, rc.cache_size, rc.num_keys, &wt);
        print_row(r_walde);
        print_footer();
        std::printf("  NOTE: LRU / W-TinyLFU capacity = %u  (= WALDE L1 %u + L2 %u)\n",
                    walde_total, rc.cache_size, rc.cache_size / 2);
        std::printf("  This isolates policy quality from capacity advantage.\n\n");
    }

    // ── Concurrency scaling ───────────────────────────────────
    run_concurrency_scaling(rc);

    // ── Trace-based workload (optional) ──────────────────────
    if (!rc.trace_file.empty()) {
        constexpr size_t kMaxTraceOps = 2000000;

        // Use the new streaming trace loader (supports CSV + simple formats)
        trace::TraceReader reader(rc.trace_file);
        if (!reader.is_open()) {
            std::printf("WARNING: Could not open trace file: %s\n", rc.trace_file.c_str());
        } else {
            auto raw_ops = trace::load_trace_file(rc.trace_file, kMaxTraceOps);
            if (raw_ops.empty()) {
                std::printf("WARNING: No valid operations in trace file: %s\n",
                            rc.trace_file.c_str());
            } else {
                // Re-open reader to get stats (load_trace_file uses its own)
                trace::TraceReader stats_reader(rc.trace_file);
                trace::TraceOp dummy;
                size_t total_in_file = 0;
                while (stats_reader.next(dummy)) ++total_in_file;

                auto trace_ops = trace::to_bench_ops(raw_ops);

                std::unordered_set<std::string> unique_keys;
                uint64_t get_count = 0, put_count = 0;
                for (const auto& op : trace_ops) {
                    unique_keys.insert(op.key);
                    if (op.type == bench::BenchOp::GET) ++get_count;
                    else ++put_count;
                }

                std::printf("┌─── Trace Benchmark ───────────────────────┐\n");
                std::printf("│  File         : %s\n", rc.trace_file.c_str());
                std::printf("│  Ops loaded   : %zu", trace_ops.size());
                if (total_in_file > trace_ops.size())
                    std::printf(" (capped from %zu)", total_in_file);
                std::printf("\n");
                std::printf("│  Unique keys  : %zu\n", unique_keys.size());
                std::printf("│  GET ops      : %lu (%.1f%%)\n",
                            static_cast<unsigned long>(get_count),
                            100.0 * get_count / trace_ops.size());
                std::printf("│  PUT ops      : %lu (%.1f%%)\n",
                            static_cast<unsigned long>(put_count),
                            100.0 * put_count / trace_ops.size());
                std::printf("│  Cache ratio  : %.2f%%\n",
                            100.0 * rc.cache_size / unique_keys.size());
                std::printf("└────────────────────────────────────────────┘\n\n");

                uint32_t trace_warmup = std::min(rc.warmup_ops,
                    static_cast<uint32_t>(trace_ops.size() / 4));
                RunConfig trc  = rc;
                trc.warmup_ops  = trace_warmup;
                trc.measure_ops = static_cast<uint32_t>(trace_ops.size()) - trace_warmup;
                trc.num_keys    = static_cast<uint32_t>(
                    std::min(unique_keys.size(), static_cast<size_t>(UINT32_MAX)));

                char label[256];
                std::snprintf(label, sizeof(label),
                    "Trace: %s  (%zu ops, %zu unique keys)",
                    rc.trace_file.c_str(), trace_ops.size(), unique_keys.size());

                // Run LRU and W-TinyLFU via the standard path
                print_header(label, trc.zipf_alpha, trc.cache_size, trc.num_keys);

                auto r_lru = run_lru(trace_ops, trc.warmup_ops, trc.cache_size);
                print_row(r_lru);

                auto r_wtlfu = run_wtinylfu(trace_ops, trc.warmup_ops, trc.cache_size);
                print_row(r_wtlfu);

                // Run WALDE with a backend populated from trace keys
                // (the standard make_backend generates key_0..key_N which
                //  don't match trace keys, causing 0% hit rate)
                walde::PathLatencyTracker walde_tracker;
                auto trace_backend = make_trace_backend(unique_keys);
                auto r_walde = run_walde_with_backend(
                    trace_ops, trc.warmup_ops, trc.cache_size,
                    std::move(trace_backend), &walde_tracker);
                print_row(r_walde);
                print_footer();

                std::printf("\n  Admission details:\n");
                print_admission_details(r_wtlfu);
                print_admission_details(r_walde);
                print_walde_breakdown(r_walde);
                std::printf("\n");
                print_walde_latency_breakdown(walde_tracker);
            }
        }
    }

    std::printf("══════════════════════════════════════════\n");
    std::printf("  Comparison benchmark complete\n");
    std::printf("══════════════════════════════════════════\n");
    return 0;
}
