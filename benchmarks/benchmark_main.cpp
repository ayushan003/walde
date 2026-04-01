#include "cache_engine.h"
#include "latency_instrumentation.h"
#include "storage_backend.h"
#include "workload_generator.h"
#include "benchmark_result.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace walde;
using Clock = std::chrono::steady_clock;

template<typename T>
void do_not_optimize(const T& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

struct EngineBundle {
    std::unique_ptr<CacheEngine> engine;
    InMemoryBackend* backend;
};

EngineBundle make_engine(const CacheEngine::Config& cfg, uint32_t num_records) {
    auto backend = std::make_unique<InMemoryBackend>();
    auto* raw = backend.get();
    for (uint32_t i = 0; i < num_records; ++i) {
        raw->put("key_" + std::to_string(i), "value_" + std::to_string(i));
    }
    auto engine = std::make_unique<CacheEngine>(cfg, std::move(backend));
    return {std::move(engine), raw};
}

void warm_up(CacheEngine& engine, uint32_t num_ops,
             uint32_t num_keys, double alpha, uint64_t seed) {
    WorkloadGenerator::Config wl;
    wl.num_keys = num_keys;
    wl.zipf_alpha = alpha;
    wl.write_ratio = 0.05;
    wl.scan_probability = 0.0;
    wl.seed = seed;
    WorkloadGenerator gen(wl);

    for (uint32_t i = 0; i < num_ops; ++i) {
        auto op = gen.next();
        if (op.type == WorkloadOp::PUT) {
            engine.put(op.key, op.value);
        } else {
            auto v = engine.get(op.key);
            do_not_optimize(v);
        }
    }
}

// ─── Core benchmark with full instrumentation ───────────────
BenchmarkResult run_instrumented(CacheEngine& engine,
                                  WorkloadGenerator& gen,
                                  uint32_t num_ops,
                                  const BenchmarkConfig& bcfg) {
    PathLatencyTracker tracker;

    uint64_t h1_before = engine.l1_hits();
    uint64_t m1_before = engine.l1_misses();
    uint64_t h2_before = engine.l2_hits();
    uint64_t ev_before = engine.l1_evictions();
    uint64_t ad_before = engine.l1_admissions();
    uint64_t rj_before = engine.l1_rejections();

    auto wall_start = Clock::now();

    for (uint32_t i = 0; i < num_ops; ++i) {
        auto op = gen.next();
        LatencyBreakdown bd;

        auto t0 = Clock::now();
        if (op.type == WorkloadOp::PUT) {
            engine.put(op.key, op.value, &bd);
        } else {
            auto v = engine.get(op.key, &bd);
            do_not_optimize(v);
        }
        auto t1 = Clock::now();

        bd.total_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(t1 - t0).count();
        tracker.record(bd);
    }

    auto wall_end = Clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();

    uint64_t bench_h1 = engine.l1_hits() - h1_before;
    uint64_t bench_m1 = engine.l1_misses() - m1_before;
    uint64_t bench_h2 = engine.l2_hits() - h2_before;

    BenchmarkResult result;
    result.config = bcfg;
    result.total_ops = num_ops;
    result.duration_sec = wall_sec;
    result.throughput_ops_sec = num_ops / wall_sec;
    result.l1_hits = bench_h1;
    result.l1_misses = bench_m1;
    result.l2_hits = bench_h2;
    result.l1_hit_rate = (bench_h1 + bench_m1 > 0)
        ? static_cast<double>(bench_h1) / (bench_h1 + bench_m1) : 0.0;
    result.l2_hit_rate = (bench_m1 > 0)
        ? static_cast<double>(bench_h2) / bench_m1 : 0.0;
    result.admissions = engine.l1_admissions() - ad_before;
    result.rejections = engine.l1_rejections() - rj_before;
    result.evictions = engine.l1_evictions() - ev_before;

    result.populate_from_tracker(tracker);
    return result;
}

void print_result(const BenchmarkResult& r) {
    std::printf("\n╔═══ %s\n", r.config.workload_type.c_str());
    std::printf("║  Throughput:     %.0f ops/sec\n", r.throughput_ops_sec);
    std::printf("║  Duration:       %.3f sec\n", r.duration_sec);
    std::printf("║  L1 hit rate:    %.1f%%\n", r.l1_hit_rate * 100);
    std::printf("║  L2 hit rate:    %.1f%% (given L1 miss)\n", r.l2_hit_rate * 100);
    std::printf("║  Admissions:     %lu\n", r.admissions);
    std::printf("║  Rejections:     %lu\n", r.rejections);
    std::printf("║  Evictions:      %lu\n", r.evictions);
    std::printf("║\n");
    std::printf("║  ── End-to-end latency ──\n");
    std::printf("║  Total  p50=%.2f  p95=%.2f  p99=%.2f  p999=%.2f μs\n",
                r.total.p50, r.total.p95, r.total.p99, r.total.p999);
    std::printf("║\n");
    std::printf("║  ── Per-path latency (p50 μs) ──\n");
    std::printf("║  L1 hit:      %.3f  (%lu ops)\n", r.l1_hit_path.p50, r.l1_hit_path.count);
    std::printf("║  L2 hit:      %.3f  (%lu ops)\n", r.l2_hit_path.p50, r.l2_hit_path.count);
    std::printf("║  Backend hit: %.3f  (%lu ops)\n", r.backend_hit_path.p50, r.backend_hit_path.count);
    std::printf("║\n");
    std::printf("║  ── Component breakdown (p50 μs) ──\n");
    std::printf("║  Lock wait:  %.3f\n", r.lock_wait.p50);
    std::printf("║  Lookup:     %.3f\n", r.lookup.p50);
    std::printf("║  Admission:  %.3f  (%lu events)\n", r.admission.p50, r.admission.count);
    std::printf("║  Eviction:   %.3f  (%lu events)\n", r.eviction.p50, r.eviction.count);
    std::printf("║  Slab alloc: %.3f\n", r.slab_alloc.p50);
    std::printf("║  L2 access:  %.3f\n", r.l2_access.p50);
    std::printf("║  Backend:    %.3f\n", r.backend_access.p50);
    std::printf("╚═══\n");
}

// ─── Multi-threaded benchmark ───────────────────────────────
BenchmarkResult run_mt_instrumented(CacheEngine& engine,
                                     uint32_t ops_per_thread,
                                     int num_threads,
                                     const WorkloadGenerator::Config& wl_cfg,
                                     const BenchmarkConfig& bcfg) {
    std::vector<PathLatencyTracker> trackers(num_threads);

    auto wall_start = Clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            auto cfg = wl_cfg;
            cfg.seed = wl_cfg.seed + t * 7919;
            WorkloadGenerator gen(cfg);

            for (uint32_t i = 0; i < ops_per_thread; ++i) {
                auto op = gen.next();
                LatencyBreakdown bd;

                auto t0 = Clock::now();
                if (op.type == WorkloadOp::PUT) {
                    engine.put(op.key, op.value, &bd);
                } else {
                    auto v = engine.get(op.key, &bd);
                    do_not_optimize(v);
                }
                auto t1 = Clock::now();

                bd.total_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(t1 - t0).count();
                trackers[t].record(bd);
            }
        });
    }

    for (auto& t : threads) t.join();

    auto wall_end = Clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();

    // Merge trackers
    PathLatencyTracker merged;
    for (auto& tr : trackers) merged.merge(tr);

    uint64_t total = static_cast<uint64_t>(ops_per_thread) * num_threads;

    BenchmarkResult result;
    result.config = bcfg;
    result.total_ops = total;
    result.duration_sec = wall_sec;
    result.throughput_ops_sec = total / wall_sec;
    result.l1_hits = engine.l1_hits();
    result.l1_misses = engine.l1_misses();
    result.l2_hits = engine.l2_hits();
    result.l1_hit_rate = engine.l1_hit_rate();
    result.l2_hit_rate = (engine.l1_misses() > 0)
        ? static_cast<double>(engine.l2_hits()) / engine.l1_misses() : 0.0;
    result.admissions = engine.l1_admissions();
    result.rejections = engine.l1_rejections();
    result.evictions = engine.l1_evictions();

    result.populate_from_tracker(merged);
    return result;
}

// ═════════════════════════════════════════════════════════════
int main() {
    std::printf("╔════════════════════════╗\n");
    std::printf("║  WALDE Benchmark Suite ║\n");
    std::printf("╚════════════════════════╝\n\n");

    constexpr uint32_t NUM_RECORDS  = 100000;
    constexpr uint32_t WARMUP_OPS   = 100000;
    constexpr uint32_t MEASURE_OPS  = 200000;
    constexpr double   ZIPF_ALPHA   = 0.99;

    CacheEngine::Config cfg;
    cfg.l1_config.total_capacity = 8192;
    cfg.l2_capacity = 4096;
    cfg.demotion_queue_size = 2048;

    // ═══ Benchmark 1: Pure Zipfian ═══
    std::printf("── Benchmark 1: Pure Zipfian baseline ──\n");
    {
        auto [engine, backend] = make_engine(cfg, NUM_RECORDS);
        warm_up(*engine, WARMUP_OPS, NUM_RECORDS, ZIPF_ALPHA, 42);
        std::printf("  Post-warmup L1 hit rate: %.1f%%\n", engine->l1_hit_rate() * 100);

        WorkloadGenerator::Config wl;
        wl.num_keys = NUM_RECORDS;
        wl.zipf_alpha = ZIPF_ALPHA;
        wl.write_ratio = 0.05;
        wl.scan_probability = 0.0;
        wl.seed = 9999;
        WorkloadGenerator gen(wl);

        BenchmarkConfig bcfg{"zipfian", cfg.l1_config.total_capacity, 1,
                              MEASURE_OPS, ZIPF_ALPHA, 0.0, ""};
        auto r = run_instrumented(*engine, gen, MEASURE_OPS, bcfg);
        print_result(r);
    }

    // ═══ Benchmark 2: Scan resistance ═══
    std::printf("\n── Benchmark 2: Scan resistance ──\n");
    {
        auto [engine, backend] = make_engine(cfg, NUM_RECORDS);
        warm_up(*engine, WARMUP_OPS, NUM_RECORDS, ZIPF_ALPHA, 42);
        std::printf("  Post-warmup L1 hit rate: %.1f%%\n", engine->l1_hit_rate() * 100);

        WorkloadGenerator::Config wl;
        wl.num_keys = NUM_RECORDS;
        wl.zipf_alpha = ZIPF_ALPHA;
        wl.write_ratio = 0.05;
        wl.scan_probability = 0.10;
        wl.scan_length = 500;
        wl.seed = 7777;
        WorkloadGenerator gen(wl);

        BenchmarkConfig bcfg{"zipfian+scan", cfg.l1_config.total_capacity, 1,
                              MEASURE_OPS, ZIPF_ALPHA, 0.10, ""};
        auto r = run_instrumented(*engine, gen, MEASURE_OPS, bcfg);
        print_result(r);

        // ── Corrected latency model ─────────────────────────
        // Use per-path p50 as conditional latencies (not aggregate percentiles)
        double h1 = r.l1_hit_rate;
        double h2 = r.l2_hit_rate;
        double t1 = r.l1_hit_path.p50;
        double t2 = r.l2_hit_path.p50;
        double tm = r.backend_hit_path.p50;

        // Fallback if a path has zero samples
        if (r.l1_hit_path.count == 0) t1 = r.total.p50;
        if (r.l2_hit_path.count == 0) t2 = r.total.p95;
        if (r.backend_hit_path.count == 0) tm = r.total.p99;

        double expected = h1 * t1 + (1 - h1) * h2 * t2 +
                          (1 - h1) * (1 - h2) * tm;

        std::printf("\n─── Latency Model (corrected: per-path conditional) ───\n");
        std::printf("  h₁=%.3f  t₁=%.3fμs (L1 hit p50, %lu samples)\n",
                    h1, t1, r.l1_hit_path.count);
        std::printf("  h₂=%.3f  t₂=%.3fμs (L2 hit p50, %lu samples)\n",
                    h2, t2, r.l2_hit_path.count);
        std::printf("  tₘ=%.3fμs (backend hit p50, %lu samples)\n",
                    tm, r.backend_hit_path.count);
        std::printf("  Expected mean: %.3f μs\n", expected);
        std::printf("  Observed mean: %.3f μs\n", r.total.mean);
        double error = (r.total.mean > 0)
            ? std::abs(expected - r.total.mean) / r.total.mean * 100 : 0;
        std::printf("  Model error: %.1f%%\n", error);
    }

    // ═══ Benchmark 3: Concurrency scaling ═══
    std::printf("\n── Benchmark 3: Concurrency scaling ──\n");
    std::vector<std::pair<int, double>> scaling;

    for (int threads : {1, 2, 4, 8}) {
        auto [engine, backend] = make_engine(cfg, NUM_RECORDS);
        warm_up(*engine, WARMUP_OPS, NUM_RECORDS, ZIPF_ALPHA, 42);

        WorkloadGenerator::Config wl;
        wl.num_keys = NUM_RECORDS;
        wl.zipf_alpha = ZIPF_ALPHA;
        wl.write_ratio = 0.05;
        wl.scan_probability = 0.0;
        wl.seed = 5555;

        // FIXED: Safe narrowing conversion
        BenchmarkConfig bcfg{"zipfian", cfg.l1_config.total_capacity,
                              static_cast<uint32_t>(threads), 100000U * static_cast<uint32_t>(threads),
                              ZIPF_ALPHA, 0.0, ""};
        auto r = run_mt_instrumented(*engine, 100000, threads, wl, bcfg);
        print_result(r);
        scaling.push_back({threads, r.throughput_ops_sec});
    }

    std::printf("\n─── Scaling Summary ───\n");
    double base = scaling[0].second;
    for (auto& [t, tput] : scaling) {
        std::printf("  %d thread(s): %.0f ops/sec (%.2fx)\n", t, tput, tput / base);
    }

    // ═══ Benchmark 4: Timer overhead ═══
    std::printf("\n── Benchmark 4: Timer overhead ──\n");
    {
        constexpr int N = 100000;
        std::vector<int64_t> ov;
        ov.reserve(N);
        for (int i = 0; i < N; ++i) {
            auto t0 = Clock::now();
            auto t1 = Clock::now();
            ov.push_back(std::chrono::duration_cast<
                std::chrono::nanoseconds>(t1 - t0).count());
        }
        std::sort(ov.begin(), ov.end());
        double mean = 0;
        for (auto v : ov) mean += v;
        mean /= N;
        std::printf("  steady_clock overhead: mean=%.0fns p50=%.0fns p99=%.0fns\n",
                    mean, (double)ov[N/2], (double)ov[N*99/100]);
        std::printf("  Per-op measurement cost: ~%.0fns (2 clock reads)\n", mean * 2);
    }
    std::printf("\n═══ Benchmark complete ═══\n");
    return 0;
}
