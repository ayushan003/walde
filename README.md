# WALDE — Concurrent Multi-Level Cache with Frequency-Based Admission

WALDE is a concurrent cache that replaces the Bloom doorkeeper from W-TinyLFU with CMS-only admission and measures the system-level consequences.

+8.8pp hit rate vs LRU under scan workloads (69.5% vs 60.7%) and +5.8pp on ARC S3, at ~2.8× lower single-thread throughput.

Removing Bloom increases admission rate 2–4×, trading eviction churn for zero reset complexity and full per-decision observability.

All results are derived from identical operation sequences across policies, with warmup excluded via counter deltas. YCSB results use cache=8192; ARC S3 uses `--cache-size 65536 --trace traces/s3_arc.txt`.

---

## Real-World Validation: ARC S3 Block I/O

WALDE is +5.8pp over LRU and +2.5pp over W-TinyLFU on production block I/O traces, with L2 contributing ~45% of total hits.

> ARC paper (Megiddo & Modha, FAST 2003), S3 dataset. 16.4M total ops, **1.062M unique keys** (verified from trace; earlier scripts over-counted). Benchmarked at 2M ops (capped), cache=65,536 (~6.2% of working set).

| Policy | Hit Rate | Throughput | p50 | p99 |
|---|---|---|---|---|
| LRU | 1.5% | 3.67M/s | 0.30 μs | 0.60 μs |
| W-TinyLFU | 4.8% | 2.86M/s | 0.30 μs | 0.60 μs |
| **WALDE** | **7.3%** | 711K/s | 1.20 μs | 2.40 μs |

WALDE detail: L1 hit=4.0%, L2 hit=3.4% (of L1 misses). 62,551 L2 hits vs 76,767 L1 hits — async demotion provides real rescue value on production access patterns (L2 ≈ 45% of total hits).

WALDE achieves this without a Bloom filter — beating W-TinyLFU's hit rate while making every admission decision individually attributable.

```bash
cd traces && bash download.sh && cd ..
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWALDE_BUILD_COMPARISON=ON
cmake --build build -j$(nproc)
./build/walde_comparison --trace traces/s3_arc.txt --cache-size 65536
```

---

## Where WALDE Lacks

| Scenario | Use Instead |
|---|---|
| Single-threaded, latency-critical path | LRU |
| Recency-biased workloads (YCSB-D, read-latest) | LRU |
| One-hit-wonder rate >50% | W-TinyLFU |
| Memory-constrained (WALDE: 2.2 MB vs LRU: 1.1 MB at 8K) | LRU or W-TinyLFU |
| Equal-capacity YCSB-B (12288 total) | W-TinyLFU (+0.2pp) |

---

## Design Overview

W-TinyLFU depends on a Bloom-filtered admission gate. WALDE removes it and measures what breaks and what improves.

- **What happens if Bloom is removed?** Admission rate rises 2–4× on skewed reads; eviction churn and L2 pressure increase proportionally.
- **Is CMS alone sufficient for scan resistance?** Yes — hit-only increment ensures scan keys never accumulate frequency, making doorkeeper rejection redundant under skewed access.
- **What breaks?** One-hit-wonder defense degrades to decay-only; shared slab allocator becomes a cross-stripe CAS bottleneck at high thread counts.
- **What improves?** Zero reset-schedule complexity; every admission decision is attributable at nanosecond resolution.

---

## Key Engineering Insights

- **Removing the Bloom doorkeeper increases admission rate 2–4× on skewed reads** (WALDE: 7.4–12.7% vs W-TinyLFU: 3.2% on YCSB-A/B/C), directly causing higher eviction pressure and L2 traffic. On YCSB-D the relationship inverts — W-TinyLFU admits 23.7% vs WALDE 15.4% — because the doorkeeper passes recency-fresh keys that hit-only CMS rejects.
- **Hit-only CMS increment eliminates scan pollution without a pre-filter.** Scan keys that never reside in L1 have frequency ≈ 1 at admission time; Probation victims have frequency ≫ 1. The gate rejects scan keys without a Bloom stage.
- **CMS width reduction 16× (8192 → 512 per stripe) produced zero hit-rate change** across all YCSB workloads. The original global sizing was a 16× memory over-allocation artifact.
- **Shared slab allocator regresses WALDE at high thread counts.** At cache=8192, WALDE drops 2.67M → 1.96M/s (4T→8T, −27%). The identification is comparative: StripedLRU — which differs primarily in lacking a shared slab — degrades far less (3.93M → 3.54M/s). At larger cache sizes (65536) where eviction is rarer, the slab bottleneck is less pronounced (WALDE 5.94M → 5.84M/s, −2%). `LatencyBreakdown.slab` is captured in single-threaded runs (p50 = 50 ns); multi-threaded slab instrumentation is future work.
- **L2 rescue tier contributes ~45% of total hits on production traces.** ARC S3: 62,551 L2 hits vs 76,767 L1 hits. On YCSB (cache=8192), L2 contributes far less (~2% of total hits) — the rescue value is workload and cache-ratio dependent.
- **YCSB-D is a hard failure case, not a tuning gap.** At Window=1%, new keys cannot build CMS frequency fast enough for recency-biased access. WALDE: 70.3% vs LRU: 72.0% (cache=8192).

---

## Positioning

- **vs LRU:** frequency-based admission prevents scan-induced cache pollution; 64-stripe sharding eliminates single-mutex contention.
- **vs W-TinyLFU:** removes Bloom filter → simpler admission model, full per-decision observability, higher churn. Slightly worse at equal total capacity (YCSB-B @ 12288: 77.8% vs 78.0%).
- **vs StripedLRU:** better hit rate under skew (+7.5pp YCSB-B at cache=8192), worse single-thread throughput due to admission gate + shared allocator overhead.

---

## Architecture

Critical path (measured): hash → stripe lock → L1 lookup → CMS admission → conditional L2 lookup.

```
  Client
    |
    |  cache.get(key) / cache.put(key, value)
    v
  CacheEngine
    |
    |  XXHash64(key) % 64                      [~1–3 ns, no contention]
    v
  CacheStripe[i]  — one of 64 independent stripes
    |
    |  lock_wait: acquire stripe mutex         [CONTENTION POINT A]
    v
  +----------------------------------------------------------+
  |  L1: Three-Segment LRU                                   |
  |                                                          |
  |  Window (1%)  →  overflow: eviction candidate ready      |
  |    v                                                     |
  |  CMS.query(candidate) vs CMS.query(victim)  [DECISION]   |
  |    |-- f(c) > f(v): candidate → Probation                |
  |    |-- f(c) ≤ f(v): candidate → DemotionQueue            |
  |                                                          |
  |  Probation (20%) — hit → promote to Protected            |
  |  Protected  (80%) — overflow → demote to Probation tail  |
  |                                                          |
  |  SlabAllocator — shared across all 64 stripes [CONTENTION POINT B]
  |  [atomic CAS free-list; bottleneck at small-cache + high threads]
  +----------------------------------------------------------+
    |
    |  eviction → DemotionQueue (2048 slots)   [get() never blocks]
    v
  DemotionDrainer (background thread)
    v
  L2Cache[j]  — j = hash % 16
    |
    |  lock_wait: acquire L2 stripe mutex      [CONTENTION POINT C]
    v
  Exclusive LRU eviction within L2 stripe
```

**Stage latency (single thread, YCSB-B, cache=8192, p50):**

| Stage       | Typical cost               | Notes                                              |
|-------------|----------------------------|----------------------------------------------------|
| `lock_wait` | 50 ns                      | Near-zero at 1–2T; scales with stripe load         |
| `lookup`    | 150 ns                     | Hash map probe + LRU pointer update                |
| `admission` | 150 ns                     | Two CMS reads + one comparison                     |
| `eviction`  | 300 ns                     | Slab dealloc + DemotionQueue enqueue               |
| `l2`        | 150 ns                     | Separate stripe lock + hash probe                  |
| `slab`      | 50 ns (single-thread)      | Atomic free-list CAS; instrumented via single-thread runs |

> `backend_ns` is also captured by `LatencyBreakdown` but excluded from this table (storage path, not cache path).

---

## Design Decisions

### 64 stripes

Power-of-two above typical server core counts (8–32); 128 items per stripe at 8,192 total — large enough for meaningful three-segment LRU state.

### CMS increment on hit only

Scan keys that miss L1 never accumulate frequency. On `put()`, they compete against Probation victims (scan ≈ 1 vs victim ≫ 1) — the gate rejects them. Under one-hit-wonder rates >50%, both candidate and victim have frequency ≈ 1 and admission degenerates toward random; this is the explicit gap Bloom solves.

### Async L1→L2 demotion

Evicted items enqueue to a bounded `DemotionQueue` (2048 slots); background `DemotionDrainer` pops batches of 64 into L2. The `get()` path never blocks on L2 insertion. At queue >80% capacity, items are dropped — degradation via lost rescue opportunities, not stalls.

### Shared slab allocator

Single `SlabAllocator` backed by tagged-CAS free-list (ABA-prevention via 32-bit tag). Cross-stripe CAS contention is cache-ratio dependent: at 8.2% fill (cache=8192, 100K keys), WALDE regresses 4T→8T by −27%; at 65.5% fill (cache=65536), regression at the same step is only −2%. Fix not implemented: per-stripe memory arenas with thread-local bump allocators.

### Window=1%, Probation=20%, Protected=80%

Ratios inherited from Caffeine/W-TinyLFU reference. Not tuned against WALDE's hit-only CMS policy.

---

## Performance

> Intel Core i7-1360P · 16 GiB RAM · Ubuntu 24.04 · GCC 13.3 `-O2 -DNDEBUG`

### Hit Rate — YCSB (cache=8192, working set=100K, Zipf α=0.99, 500K ops, 100K warmup, 3 runs)

| Workload | LRU (8192) | W-TinyLFU (8192) | WALDE L1-only | WALDE Overall (L1+L2) |
|---|---|---|---|---|
| YCSB-A (50r/50w) | 70.2% | 75.0% | 75.8% | 77.9% |
| YCSB-B (95r/5w) | 70.3% | 75.1% | 76.1% | 77.8% |
| YCSB-C (100r) | 70.3% | 75.1% | 76.1% | 77.7% |
| YCSB-D (read-latest) | **72.0%** | 74.8% | 67.1% | 70.3% |
| YCSB-F (RMW) | 70.3% | 75.2% | 75.6% | 77.8% |
| Scan (10%) | 60.7% | 66.7% | 68.0% | **69.5%** |
| Uniform | 8.2% | 8.1% | 8.2% | 12.3% |

Run-to-run variance (YCSB-B, 3 seeds): LRU 70.40% ± 0.08%, W-TinyLFU 75.28% ± 0.14%, WALDE 77.86% ± 0.08%.

Equal-capacity check: WALDE (8192 L1 + 4096 L2 = 12288 total) = 77.8% vs W-TinyLFU (12288) = 78.0%. L2 tiering does not outperform equivalent single-tier capacity at this working-set ratio.

### Throughput — YCSB-B, 3-run mean ± std (cache=8192)

| Policy | Throughput | p50 | p95 | p99 | Memory |
|---|---|---|---|---|---|
| LRU | **4.19M ± 77K/s** | 0.15 μs | 0.60 μs | 0.60 μs | 1,056 KB |
| W-TinyLFU | 3.31M ± 16K/s | 0.30 μs | 0.60 μs | 0.60 μs | 1,192 KB |
| WALDE | 1.49M ± 22K/s | 0.30 μs | 2.40 μs | 2.40 μs | 2,200 KB |

### Concurrency Scaling — YCSB-B (cache=8192)

Slab contention is the dominant factor at small cache/working-set ratios. StripedLRU (no shared slab) is the control.

| Threads | LRU | StripedLRU | W-TinyLFU | WALDE |
|---|---|---|---|---|
| 1 | 2.22M/s | 2.19M/s | 2.00M/s | 1.52M/s |
| 2 | 1.07M/s | 2.51M/s | 965K/s | 1.99M/s |
| 4 | 982K/s | **3.93M/s** | 934K/s | **2.67M/s** |
| 8 | 692K/s | 3.54M/s | 659K/s | 1.96M/s |

WALDE 4T→8T: −27%. StripedLRU 4T→8T: −10%. The gap is the shared slab.

At larger cache size (cache=65536, 65.5% fill), eviction is rarer and the slab bottleneck largely disappears:

| Threads | LRU | StripedLRU | W-TinyLFU | WALDE |
|---|---|---|---|---|
| 1 | 3.83M/s | 3.66M/s | 3.19M/s | 2.99M/s |
| 2 | 2.76M/s | 5.11M/s | 2.15M/s | 4.07M/s |
| 4 | 2.31M/s | 7.59M/s | 1.90M/s | 5.94M/s |
| 8 | 1.67M/s | **9.36M/s** | 1.43M/s | **5.84M/s** |

WALDE 4T→8T at cache=65536: −2% (vs −27% at cache=8192). The slab bottleneck is eviction-rate driven, not inherent to thread count.

### Admission Gate Behavior (cache=8192)

| Workload | WALDE admit | WALDE reject | W-TinyLFU admit | WALDE evictions |
|---|---|---|---|---|
| YCSB-A | 12.7% | 87.3% | 3.2% | 15,316 |
| YCSB-B | 7.9% | 92.1% | 3.2% | 9,438 |
| YCSB-C | 7.4% | 92.6% | 3.2% | 8,867 |
| YCSB-D | 15.4% | 84.6% | **23.7%** | 27,863 |
| YCSB-F | 19.1% | 80.9% | 13.5% | 23,287 |
| Scan (10%) | 8.6% | 91.4% | 2.8% | 9,247 |
| Uniform | 6.1% | 93.9% | 3.7% | 27,767 |

---

## Explicit Tradeoffs

- **Higher admission rate (2–4× vs W-TinyLFU on skewed reads)** increases eviction churn and L2 pressure. One-hit-wonder defense is decay-only.
- **Shared slab allocator introduces cross-stripe CAS contention**, with severity proportional to eviction rate (i.e., cache/working-set ratio).
- **Recency-biased workloads penalized by admission gate:** −1.7pp vs LRU on YCSB-D overall (−4.9pp L1-only).
- **L2 uses `std::list`:** per-node heap allocation, poor cache locality. Not addressed.
- **At equal total capacity (12288), W-TinyLFU narrowly beats WALDE** (78.0% vs 77.8% YCSB-B).

---

## Policy Comparison

| Property                | LRU    | W-TinyLFU            | WALDE                     |
|-------------------------|--------|----------------------|---------------------------|
| Admission gate          | None   | CMS + Bloom          | CMS only                  |
| Scan resistance         | None   | Strong               | Strong                    |
| Bloom reset required    | No     | Yes                  | No                        |
| One-hit-wonder defense  | None   | Strong               | Partial (decay only)      |
| Per-path observability  | None   | None                 | Nanosecond, per call      |
| Multi-level tiering     | No     | No                   | L1 + L2                   |
| Operational complexity  | Low    | Moderate             | Low                       |

---

## Learnings

- Admission policies shift bottlenecks rather than eliminate them — removing Bloom moved the constraint to memory allocation.
- Bloom filtering is not required for scan resistance — CMS alone is sufficient under skewed access.
- The slab CAS bottleneck is eviction-rate driven, not a fixed thread-count cliff: at 65.5% fill, WALDE scales near-linearly to 8 threads.
- Real traces invalidate assumptions from synthetic benchmarks: L2 rescue contributed ~45% of hits on ARC S3, but only ~2% on YCSB (8.2% fill).
- Doorkeeper trade-offs are workload-dependent: YCSB-D shows W-TinyLFU admitting *more* than WALDE (23.7% vs 15.4%) — the Bloom filter passes recency-fresh keys that hit-only CMS rejects.

---

## Future Work

- Per-stripe memory arenas with thread-local bump allocators to eliminate slab CAS contention at high eviction rates.
- Wire `LatencyBreakdown` instrumentation into the multi-threaded concurrency-scaling benchmark to directly observe `slab_ns` degradation at 8T (currently captured only on single-threaded runs).
- Optional Bloom doorkeeper as a compile-time flag to evaluate one-hit-wonder defense in isolation.
- Tune Window/Probation/Protected ratios against WALDE's hit-only CMS policy; current ratios inherit Caffeine defaults.
- Extend trace validation to Twitter cache, Wikimedia CDN, and OLTP datasets.

---

## Build & Reproduce

```bash
git clone https://github.com/ayushan003/walde && cd walde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure   # 99 tests

# YCSB benchmark (3-run variance)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWALDE_BUILD_COMPARISON=ON
cmake --build build -j$(nproc)
./build/walde_comparison --runs 3

# ARC S3 trace benchmark
cd traces && bash download.sh && cd ..
./build/walde_comparison --trace traces/s3_arc.txt --cache-size 65536
```

All YCSB results: `walde_comparison --runs 3` with seeds `42 + run × 7919`; warmup excluded via counter deltas.
ARC S3 results: 2M ops capped from 16.4M-op trace, 1.062M unique keys, cache=65,536.

---

## Project Structure

```
include/      Public API (cache_engine.h, trace_loader.h)
src/          Core implementation + trace loader (~850 lines)
benchmarks/   LRU, StripedLRU, W-TinyLFU baselines + harness + trace bridge
tests/        99 unit tests (GoogleTest), ~2,000 lines
traces/       Sample traces + ARC S3 download script
docs/         Architecture notes
```

**Stack:** C++17 · XXHash64 · Count-Min Sketch · GoogleTest · CMake

---

## References

- Einziger, G., Friedman, R., & Manes, B. (2017). TinyLFU: A highly efficient cache admission policy. *ACM Transactions on Storage*, 13(4).
- Cormode, G., & Muthukrishnan, S. (2005). An improved data stream summary: the count-min sketch and its applications. *Journal of Algorithms*, 55(1), 58–75.

## License

MIT
