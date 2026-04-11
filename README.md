# WALDE — Concurrent Multi-Level Cache with Frequency-Based Admission

WALDE is a concurrent cache that replaces the Bloom doorkeeper from W-TinyLFU with CMS-only admission and measures the system-level consequences.

+7.3pp hit rate vs LRU under scan workloads (68.0% vs 60.7%) and +5.5pp on ARC S3, at 2.9× lower single-thread throughput.

Removing Bloom increases admission rate 3–4×, trading eviction churn for zero reset complexity and full per-decision observability.

All results are derived from identical operation sequences across policies, with warmup excluded via counter deltas.

---

## Real-World Validation: ARC S3 Block I/O

This is not a synthetic gain — WALDE is +5.5pp over LRU and +2.4pp over W-TinyLFU on production block I/O traces, with L2 contributing ~50% of total hits.

> ARC paper (Megiddo & Modha, FAST 2003), S3 dataset. 16.4M total ops, 1.69M unique keys. Benchmarked at 2M ops (capped), cache=65,536 (~6.2% of working set).

| Policy | Hit Rate | Throughput | p50 | p99 |
|---|---|---|---|---|
| LRU | 1.5% | 3.77M/s | 0.30 μs | 0.60 μs |
| W-TinyLFU | 4.6% | 2.90M/s | 0.30 μs | 0.60 μs |
| **WALDE** | **7.0%** | 640K/s | 1.20 μs | 4.80 μs |

WALDE detail: L1 hit=3.9%, L2 hit=3.3% (of L1 misses). 62,565 L2 hits vs 77,818 L1 hits — async demotion provides real rescue value on production access patterns.

WALDE achieves this without a Bloom filter — matching W-TinyLFU's hit rate while making every admission decision individually attributable.

```bash
cd traces && bash download.sh && ./build/walde_comparison --trace traces/s3_arc.txt --cache-size 65536
```

---

## Where WALDE Lacks

WALDE is not a general-purpose replacement — it loses in the following regimes:

| Scenario | Use Instead |
|---|---|
| Single-threaded, latency-critical path | LRU |
| Recency-biased workloads (YCSB-D, read-latest) | LRU |
| One-hit-wonder rate >50% | W-TinyLFU |
| Memory-constrained (WALDE: 2.2 MB vs LRU: 1.1 MB at 8K) | LRU or W-TinyLFU |
| >4 threads, strict tail-latency requirements | StripedLRU (or WALDE with per-stripe arenas) |

---

## Design Overview

W-TinyLFU depends on a Bloom-filtered admission gate. WALDE removes it and measures what breaks and what improves.

- **What happens if Bloom is removed?** Admission rate rises 3–4×; eviction churn and L2 pressure increase proportionally.
- **Is CMS alone sufficient for scan resistance?** Yes — hit-only increment ensures scan keys never accumulate frequency, making doorkeeper rejection redundant under skewed access.
- **What breaks?** One-hit-wonder defense degrades to decay-only; shared slab allocator becomes a cross-stripe CAS bottleneck at ≥8 threads.
- **What improves?** Zero reset-schedule complexity; every admission decision is attributable at nanosecond resolution.

---

## Key Engineering Insights

- **Removing the Bloom doorkeeper increases admission rate 3–4×** (WALDE: 7.9–12.7% vs W-TinyLFU: 2.8–3.7%), directly causing 2–3× higher eviction pressure and L2 traffic.
- **Hit-only CMS increment eliminates scan pollution without a pre-filter.** Scan keys that never reside in L1 have frequency ≈ 1 at admission time; Probation victims have frequency ≫ 1. The gate rejects scan keys without a Bloom stage.
- **CMS width reduction 16× (8192 → 512 per stripe) produced zero hit-rate change** across all YCSB workloads. The original global sizing was a 16× memory over-allocation artifact.
- **Shared slab allocator is the dominant bottleneck at ≥8 threads.** WALDE regresses from 2.51M → 1.29M/s (4T→8T); StripedLRU (no shared slab) sustains 4.41M/s at 8T. Confirmed directly via `LatencyBreakdown.slab`, not inferred from aggregate throughput.
- **L2 rescue tier contributes ~50% of total hits on production traces.** ARC S3: L2 hit=3.3% vs L1 hit=3.9%. YCSB-B: L2 adds 1.6pp (8,009 hits / 500K ops).
- **YCSB-D is a hard failure case, not a tuning gap.** At Window=1%, new keys cannot build CMS frequency fast enough for recency-biased access. WALDE: 67.1% vs LRU: 72.0%.

---

## Positioning

- **vs LRU:** frequency-based admission prevents scan-induced cache pollution; 64-stripe sharding eliminates single-mutex contention.
- **vs W-TinyLFU:** removes Bloom filter → simpler admission model, full per-decision observability, higher churn. Same hit rate at equal capacity (YCSB-B: 77.8% vs 78.0%).
- **vs StripedLRU:** better hit rate under skew (+1.6pp YCSB-B), worse throughput due to admission gate + shared allocator overhead.

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
  |  CMS.query(candidate) vs CMS.query(victim)  [DECISION: admission]
  |  [~5–15 ns, per-stripe sketch, width=512]                |
  |    |-- f(c) > f(v): candidate → Probation                |
  |    |-- f(c) ≤ f(v): candidate → DemotionQueue            |
  |                                                          |
  |  Probation (20%) — hit → promote to Protected            |
  |  Protected  (80%) — overflow → demote to Probation tail  |
  |                                                          |
  |  SlabAllocator — shared across all 64 stripes [CONTENTION POINT B]
  |  [atomic CAS free-list; confirmed bottleneck at 8T]      |
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

**Stage latency (single thread, YCSB-B):**

| Stage       | Typical cost                    | Notes                                      |
|-------------|---------------------------------|--------------------------------------------|
| `lock_wait` | 0–500 ns                        | Near-zero at 1–2T; scales with stripe load |
| `lookup`    | 50–200 ns                       | Hash map probe + LRU pointer update        |
| `admission` | 5–15 ns                         | Two CMS reads + one comparison             |
| `eviction`  | 10–50 ns                        | Slab dealloc + DemotionQueue enqueue       |
| `l2`        | 200–600 ns                      | Separate stripe lock + hash probe          |
| `slab`      | 10–30 ns (1–4T); degrades at 8T | Atomic free-list CAS; confirmed bottleneck |

---

## Design Decisions

### 64 stripes

Power-of-two above typical server core counts (8–32); 128 items per stripe at 8,192 total — large enough for meaningful three-segment LRU state. At 256 stripes, protected-segment overflow increases demotion churn. Results reflect default configuration.

### CMS increment on hit only

Scan keys that miss L1 never accumulate frequency. On `put()`, they compete against Probation victims (scan ≈ 1 vs victim ≫ 1) — the gate rejects them. Under one-hit-wonder rates >50%, both candidate and victim have frequency ≈ 1 and admission degenerates toward random; this is the explicit gap Bloom solves.

### Async L1→L2 demotion

Evicted items enqueue to a bounded `DemotionQueue` (2048 slots); background `DemotionDrainer` pops batches of 64 into L2. The `get()` path never blocks on L2 insertion. At queue >80% capacity, items are dropped — degradation via lost rescue opportunities, not stalls. Eviction p50 = 0.30 μs includes slab dealloc + enqueue, not L2 insertion.

### Shared slab allocator

Single `SlabAllocator` backed by lock-free tagged-CAS free-list. Cross-stripe CAS contention limits scaling: 4T→8T regression (2.51M → 1.29M/s) confirmed via `LatencyBreakdown.slab`. Fix not implemented: per-stripe memory arenas with thread-local bump allocators.

### Window=1%, Probation=20%, Protected=80%

Ratios inherited from Caffeine/W-TinyLFU reference. Not tuned against WALDE's hit-only CMS policy — YCSB-D result (67.1% vs LRU 72.0%) reflects this directly.

---

## Performance

> Intel Core i7-1360P · 16 GiB RAM · Ubuntu 24.04 · GCC 13.3 `-O2 -DNDEBUG` · L1=8,192, L2=4,096, working set=100K keys, Zipfian α=0.99, 500K ops, 100K warmup, 3 runs. Identical pre-generated operation sequences across all policies; per/post counter deltas exclude warmup.

### Hit Rate

WALDE matches or exceeds W-TinyLFU on skewed workloads but fails on recency-biased access (YCSB-D: −4.9pp vs LRU).

| Workload | LRU (8192) | W-TinyLFU (8192) | WALDE L1-only (8192) | WALDE L1+L2 (12288) |
|---|---|---|---|---|
| YCSB-A (50r/50w) | 70.2% | 75.0% | 75.8% | 77.9% |
| YCSB-B (95r/5w) | 70.3% | 75.1% | 76.1% | 77.8% |
| YCSB-C (100r) | 70.3% | 75.1% | 76.1% | 77.7% |
| YCSB-D (read-latest) | **72.0%** | 74.8% | 67.1% | 70.3% |
| YCSB-F (RMW) | 70.3% | 75.2% | 75.6% | 77.8% |
| Scan (10%) | 60.7% | 66.7% | **68.0%** | 69.5% |
| Uniform | 8.2% | 8.1% | 8.2% | 12.3% |

Equal-capacity check (YCSB-B): WALDE (8192 L1 + 4096 L2) = 77.8% vs W-TinyLFU (12288) = 78.0%. L2 tiering does not outperform equivalent single-tier capacity.

### Throughput

WALDE trades ~3× single-thread throughput for frequency-stable eviction.

| Policy | Throughput | p50 | p95 | p99 | Memory |
|---|---|---|---|---|---|
| LRU | **4.05M ± 25K/s** | 0.15 μs | 0.60 μs | 0.60 μs | 1,056 KB |
| W-TinyLFU | 3.09M ± 45K/s | 0.30 μs | 0.60 μs | 0.60 μs | 1,192 KB |
| WALDE | 1.41M ± 29K/s | 0.30 μs | 2.40 μs | 2.40 μs | 2,200 KB |

Overhead breakdown (YCSB-B, single-thread, estimated): CMS increment ~27%, `unordered_map` lookup ~18%, instrumentation ~13%, backend lookup ~11%, `std::string` copy ~9%, L2 lookup ~6%.

### Concurrency Scaling

Lock striping scales; shared slab allocator becomes dominant bottleneck at ≥8 threads.

| Threads | LRU | StripedLRU | W-TinyLFU | WALDE |
|---|---|---|---|---|
| 1 | 1.94M/s | 2.02M/s | 1.82M/s | 1.47M/s |
| 2 | 1.08M/s | 2.50M/s | 918K/s | 1.87M/s |
| 4 | 976K/s | 3.81M/s | 908K/s | 2.51M/s |
| 8 | 627K/s | **4.41M/s** | 462K/s | 1.29M/s |

Single-mutex LRU and W-TinyLFU collapse under contention (LRU −68%, W-TinyLFU −75% by 8T) — a locking problem, not a policy problem. WALDE's 4T→8T regression is the slab allocator.

### Admission Gate Behavior

Removing Bloom increases admission rate 3–4× without improving hit rate proportionally.

| Workload | WALDE admit | WALDE reject | W-TinyLFU admit | WALDE evictions |
|---|---|---|---|---|
| YCSB-A | 12.7% | 87.3% | 3.2% | 15,316 |
| YCSB-B | 7.9% | 92.1% | 3.2% | 9,438 |
| Scan (10%) | 8.6% | 91.4% | 2.8% | 9,247 |
| Uniform | 6.1% | 93.9% | 3.7% | 27,767 |

**Summary:** WALDE improves hit rate under skew and scan pressure, but shifts the system bottleneck from eviction policy to memory allocation and contention.

---

## Explicit Tradeoffs

- **Higher admission rate (3–4× vs W-TinyLFU)** increases eviction churn and L2 pressure. One-hit-wonder defense is decay-only.
- **Shared slab allocator introduces cross-stripe CAS contention**, limiting scaling beyond 4 threads (2.51M → 1.29M/s at 4T→8T).
- **Recency-biased workloads penalized by admission gate:** new keys can't build frequency fast enough; −4.9pp vs LRU on YCSB-D.
- **L2 uses `std::list`:** per-node heap allocation, poor cache locality. Not addressed.

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
- At ≥8 threads, allocator contention dominates policy cost — eviction strategy becomes secondary.
- Real traces invalidate assumptions from synthetic benchmarks: L2 rescue value was marginal on YCSB but critical on ARC S3.

---

## Future Work

- Per-stripe memory arenas with thread-local bump allocators to eliminate slab CAS contention at 8+ threads.
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

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWALDE_BUILD_COMPARISON=ON
cmake --build build -j$(nproc)
./build/walde_comparison --runs 3
```

All YCSB results: `walde_comparison --runs 3` on hardware above. Pre-generated sequences shared across all policies; seeds `42 + run × 7919`; warmup excluded via counter deltas.

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
