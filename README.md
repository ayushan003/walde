# WALDE — A Concurrent, Multi-Level C++ Cache with Frequency-Based Admission

WALDE **(Window Admission LFU Data Engine)** is a high-performance in-memory cache written in C++17. It combines frequency-gated admission, a three-segment LRU eviction policy, async two-level tiering, and per-operation nanosecond instrumentation — all sharded across 64 independent stripes for concurrency.

> *LRU evicts your hot data during scans. WALDE doesn't.* 

---

## Why This Exists

Standard LRU has a fatal flaw for storage-adjacent workloads: **it has no admission gate.** When a scan runs — LSM compaction reads, full-table scans, batch ETL — every cold key it touches evicts a hot key unconditionally. LRU cannot tell them apart.

The fix is frequency-based admission: only let an item enter the main cache if it's accessed more often than the item it would displace. This is the core idea behind TinyLFU and W-TinyLFU.

WALDE implements a variant called **W-LFU** — the same window-based admission structure as W-TinyLFU, minus the Bloom doorkeeper. This trades some one-hit-wonder defense for operational simplicity and dramatically better observability. No Bloom reset schedule. No hidden reset-induced latency spikes. **Every admission decision is measurable at nanosecond granularity.**

---

## What This System Does

- **Frequency-gated admission (Count-Min Sketch).** When an item is evicted from the Window segment, it competes against the Probation tail by CMS frequency estimate. The lower-frequency item loses — rejected or demoted to L2. No Bloom pre-filter.

- **Three-segment LRU (Window / Probation / Protected).** Window holds 1% of capacity and acts as a staging area. Survivors graduate to Probation (20%). Repeated hits in Probation promote to Protected (80%). Protected overflow demotes to Probation tail — nothing goes directly from Protected to L2.

- **Async L1 → L2 demotion.** Evicted items are enqueued into a bounded demotion queue serviced by a background thread. The `get()` path never blocks on demotion. L2 is a 16-stripe exclusive LRU with a pluggable `StorageBackend` interface below it.

- **64-stripe sharding.** Keys are routed via XXHash64 to one of 64 independent stripes, each with its own mutex, LRU state, and CMS sketch. Stripe-level isolation is the primary concurrency mechanism — no global lock anywhere in the hot path.

- **CMS decay.** Every 5×capacity operations per stripe, all sketch counters are halved. Stripe-local — high-traffic stripes decay faster, which is correct.

- **Per-operation latency instrumentation.** Pass a non-null `LatencyBreakdown*` to `get()` or `put()` for nanosecond-resolution decomposition across six stages: `lock_wait`, `lookup`, `admission`, `eviction`, `l2`, `slab`. Pass null for zero overhead (~86 ns per call when enabled).

---

## What Makes It Strong

**Scan resistance without a Bloom filter.**
Hot keys accumulate frequency; scan keys don't. Under a 10% scan workload, the admission reject rate holds at ~80% — identical to a clean Zipfian run. The gate works.

**Every cache decision is observable.**
`LatencyBreakdown` exposes six stages — lock, lookup, admission, eviction, L2, slab — per call, in nanoseconds. No other LRU or TinyLFU implementation does this. Latency anomalies are attributable, not inferred.

**Fewer moving parts than W-TinyLFU.**
No Bloom filter means no reset schedule, no saturation tuning, no reset-induced latency spikes. Comparable hit rates on stable hot sets. Simpler to operate, simpler to reason about.

**Parallel by default — no global lock.**
64 independent stripes. Two threads on different keys never contend. Throughput scales 2.0–2.7× at 4 threads. The 8-thread regression is a slab allocator issue, not a design issue.

---

## Performance

> Intel Core i7-1360P · 16 GiB RAM · Ubuntu 24.04 · GCC `-O2 -DNDEBUG` · L1=8192, L2=4096, Zipfian α=0.99

**Hit rates**

| Workload              | L1 Hit Rate | L2 Hit Rate | Reject Rate |
|-----------------------|-------------|-------------|-------------|
| Zipfian α=0.99        | **75.3%**   | 8.7%        | ~81%        |
| Zipfian + 10% scan    | **66.6%**   | 5.5%        | ~80%        |

Reject rate is stable under scan — the admission gate holds.

**Throughput scaling**

| Threads | Throughput       | Speedup |
|---------|------------------|---------|
| 1       | 650–840K ops/sec | 1.00×   |
| 2       | 1.0–1.3M ops/sec | 1.4–1.7×|
| 4       | **1.5–2.0M ops/sec** | **2.0–2.7×** |
| 8       | 1.0–1.5M ops/sec | regresses — slab CAS contention |

**Latency (single thread)**

| p50    | p95        | p99        | p999         |
|--------|------------|------------|--------------|
| 0.6 μs | 2.4–4.8 μs | 4.8–9.6 μs | 19.2–38.4 μs |

L1 hit ~0.3–0.6 μs · L2 hit ~2.4 μs · p999 spread is lock jitter, not algorithm cost.

---

## Architecture

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
  |  Window (1% capacity)                                    |
  |    |                                                     |
  |    |  on overflow: eviction candidate ready              |
  |    v                                                     |
  |  CMS.query(candidate) vs CMS.query(victim)    [DECISION: admission]
  |  [~5–15 ns per call, per-stripe sketch]                  |
  |    |                                                     |
  |    |-- f(c) > f(v): candidate → Probation                |
  |    |                victim    → DemotionQueue            |
  |    |-- f(c) ≤ f(v): candidate → DemotionQueue            |
  |                     victim retained in Probation         |
  |                                                          |
  |  Probation (20%) — hit → promote to Protected            |
  |  Protected  (80%) — overflow → demote to Probation tail  |
  |                                                          |
  |  SlabAllocator — shared across all 64 stripes [CONTENTION POINT B]
  |  [atomic CAS free-list; bottleneck at 8T]                |
  +----------------------------------------------------------+
    |
    |  eviction → DemotionQueue (bounded)      [get() never blocks]
    v
  DemotionDrainer — background thread
    v
  L2Cache[j]  — j = hash % 16
    |
    |  lock_wait: acquire L2 stripe mutex      [CONTENTION POINT C]
    v
  Exclusive LRU eviction within L2 stripe
    v
  StorageBackend — pluggable interface
```

**Stage latency reference:**

| Stage       | Typical cost     | Notes                                              |
|-------------|------------------|----------------------------------------------------|
| `lock_wait` | 0–500 ns         | Near-zero at 1–2 threads; scales with stripe load  |
| `lookup`    | 50–200 ns        | Hash map probe + LRU pointer update                |
| `admission` | 5–15 ns          | Two CMS reads + one comparison                     |
| `eviction`  | 10–50 ns         | Slab dealloc + DemotionQueue enqueue               |
| `l2`        | 200–600 ns       | Separate stripe lock + hash probe                  |
| `slab`      | 10–30 ns (1–4T); degrades at 8T | Atomic free-list CAS; confirmed bottleneck |

---

## Where It Fails

**High one-hit-wonder rate (>50% unique, non-recurring keys).**

This is the primary failure mode, and it's structural.

- Every first-seen item enters the Window LRU and increments its CMS cell.
- On eviction, a one-hit candidate (frequency ≈1) competes against a Probation victim that is also frequency ≈1.
- The admission decision degenerates toward random selection between two low-frequency items.
- CMS decay provides no help — there is no stable hot set to protect.

**Expected outcome:** Hit rate converges to ~`capacity / working_set_size`. For a working set 10× cache capacity, that's ~10% L1 hit rate regardless of policy.

**Why W-TinyLFU is better here:** The Bloom doorkeeper rejects first-time items outright, preserving sketch integrity for items that genuinely recur. Without it, high-cardinality uniform access saturates CMS cells before decay clears them, and all frequency estimates converge toward uniform low values.

**Threshold:** If one-hit-wonder rate exceeds ~50% of total operations, use W-TinyLFU. WALDE makes no claim of correctness for this workload class.

**Known bottlenecks:**

| Issue                 | Impact                                     | Fix                          | Status          |
|-----------------------|--------------------------------------------|------------------------------|-----------------|
| Global slab free-list | Throughput regresses at 8T (CAS contention) | Per-stripe memory arenas    | Not implemented |
| No Bloom doorkeeper   | Sketch pollution under high-cardinality uniform workloads | Optional counting Bloom admission stage | Not implemented |
| In-memory only        | No disk/NVMe backend in benchmarks         | Pluggable `StorageBackend`   | Not implemented |
| Single-node           | No replication or distributed coordination | Out of scope                 | —               |

---

## Key Engineering Insights

**Bloom filter is a deliberate tradeoff.**
It narrows the workload envelope (see: Where It Fails) but eliminates reset scheduling and makes admission latency fully attributable. For stable hot sets, the hit rates hold. 

**Observability is a first-class output.**
`breakdown.slab` spiking at 8 threads is how the allocator bottleneck was identified — not from aggregate throughput regression alone. 

---

## Policy Comparison

| Property                | LRU    | TinyLFU / W-TinyLFU | WALDE (W-LFU)             |
|-------------------------|--------|----------------------|--------------------------|
| Admission gate          | None   | CMS + Bloom          | CMS only                 |
| Window segment          | No     | W-TinyLFU: Yes (1%)  | Yes (1%)                 |
| Scan resistance         | None   | Strong               | Strong                   |
| Bloom doorkeeper        | No     | Yes                  | No                       |
| Bloom reset required    | No     | Yes                  | No                       |
| One-hit-wonder defense  | None   | Strong               | Partial (decay only)     |
| Per-path observability  | None   | None                 | Nanosecond, per call     |
| Multi-level tiering     | No     | No                   | L1 + L2 + StorageBackend |
| Operational complexity  | Low    | Moderate             | Low                      |

---

## Usage

```cpp
#include "cache_engine.h"

// L1=8192, L2=4096, window fraction=1%
CacheEngine cache(8192, 4096, 0.01);

cache.put("user:1001", payload);

auto result = cache.get("user:1001");
if (result) { /* L1 or L2 hit */ }

// With instrumentation (~86 ns overhead)
LatencyBreakdown breakdown;
auto result = cache.get("user:1001", &breakdown);
// breakdown.lock_wait, .lookup, .admission, .eviction, .l2, .slab — all in nanoseconds
// Unvisited stages are 0. Pass nullptr for zero overhead.
```

---

## Build

```bash
git clone https://github.com/ayushan003/walde && cd walde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

**Benchmarks:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWALDE_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
./build/walde_benchmark
```

---

## Project Structure

```
include/      Public API headers
src/          Core implementation
benchmarks/   Benchmark harness (GoogleBenchmark)
tests/        Unit tests (GoogleTest)
docs/         Architecture notes
```

---

## Tech Stack

C++17 · XXHash64 · Count-Min Sketch · GoogleTest · GoogleBenchmark · CMake

---

## References

- Einziger, G., Friedman, R., & Manes, B. (2017). TinyLFU: A highly efficient cache admission policy. *ACM Transactions on Storage*, 13(4).
- Cormode, G., & Muthukrishnan, S. (2005). An improved data stream summary: the count-min sketch and its applications. *Journal of Algorithms*, 55(1), 58–75.

---

## License

MIT
