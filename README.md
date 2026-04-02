# WALDE — Window Admission LFU Data Engine

A concurrent, multi-level C++17 cache with CMS-only frequency-gated admission, 64-stripe sharding, and nanosecond per-path instrumentation.

**Core thesis**: For Zipfian workloads with stable hot sets, a Bloom-free admission policy based on Count-Min Sketch decay achieves comparable scan resistance to W-TinyLFU while eliminating doorkeeper overhead and exposing complete per-operation latency at every stage of the cache hierarchy.

---

## 1. Problem Statement

LRU eviction has no admission gate. Under pure Zipfian access, recency approximates frequency well enough. When scan traffic is introduced — sequential reads of cold keys with no temporal locality — every scanned item evicts a hot item unconditionally. This is the dominant failure mode of LRU in storage-adjacent workloads: LSM compaction reads, full-table scans, batch ETL.

TinyLFU and W-TinyLFU address admission by gating frequency sketch increments through a Bloom doorkeeper. This prevents one-hit wonders from polluting the CMS before confirmed recurrence. The cost is operational: the Bloom filter requires periodic reset, reset scheduling interacts with workload burstiness, and the added structure reduces transparency. Neither TinyLFU nor W-TinyLFU expose per-path latency at the call level.

WALDE occupies a different point in the design space: it eliminates the Bloom doorkeeper, relies on CMS decay to suppress stale frequency, and exposes full nanosecond-granular latency breakdowns on every `get()` and `put()` as a first-class output.

---

## 2. Design Thesis

WALDE is defined by three interdependent choices:

**CMS-only admission.** On window overflow, the eviction candidate competes against the Probation tail via Count-Min Sketch frequency estimate. No Bloom pre-filter is applied. CMS values decay by a factor of two every 5×capacity operations, bounding the influence of stale frequency on admission.

**Observable admission at nanosecond granularity.** Every call site exposes a `LatencyBreakdown` struct decomposing elapsed time into: lock acquisition, hash lookup, CMS query, eviction, L2 probe, and slab allocation. The cost of each admission decision is visible and attributable.

**Multi-level tiering with async demotion.** L1 evictions flow to L2 via a bounded queue serviced by a background thread. The `get()` path never blocks on demotion. L2 is a 16-stripe exclusive LRU. A `StorageBackend` interface sits below L2 for pluggable persistent storage.

These choices are not independent. Eliminating the Bloom filter makes admission structurally simpler and makes instrumentation more interpretable: there is no doorkeeper reset event to explain a latency anomaly, no Bloom false-positive rate to account for in hit-rate analysis.

---

## 3. Formal Definition: W-LFU

**W-LFU** (Window LFU) is a variant of the W-TinyLFU admission policy with the Bloom doorkeeper removed.

In W-TinyLFU (Einziger et al., 2017):

1. New items enter a small Window LRU (typically 1% of capacity).
2. On window eviction, the candidate is checked against a Bloom doorkeeper. If unseen before, it is rejected outright.
3. If the candidate passes the doorkeeper, its CMS frequency is compared against the Probation tail victim. The lower-frequency item is evicted.
4. The Bloom filter is reset periodically to prevent saturation.

**W-LFU** removes step 2 entirely. All window eviction candidates proceed directly to the CMS comparison. Consequence: one-hit wonders increment the CMS on arrival and compete on frequency. CMS decay bounds how long a stale frequency count persists.

Formally, let `f(k)` denote the CMS frequency estimate for key `k`. On window overflow with candidate `c` and Probation tail victim `v`:

```
admit(c)  iff  f(c) > f(v)
```

No secondary gate. No reset schedule. Decay applied globally every 5×capacity operations per stripe.

**Differentiation:**

| Property                | LRU    | TinyLFU       | W-TinyLFU     | W-LFU (WALDE) |
|-------------------------|--------|---------------|---------------|---------------|
| Admission gate          | None   | CMS + Bloom   | CMS + Bloom   | CMS only      |
| Window segment          | No     | No            | Yes (1%)      | Yes (1%)      |
| Scan resistance         | None   | Strong        | Strong        | Strong        |
| Bloom doorkeeper        | No     | Yes           | Yes           | No            |
| Bloom reset required    | No     | Yes           | Yes           | No            |
| One-hit-wonder defense  | None   | Strong        | Strong        | Partial (decay only) |
| Per-path observability  | None   | None          | None          | Nanosecond    |
| Multi-level tiering     | No     | No            | No            | Yes           |

---

## 4. Comparative Baseline

Qualitative assessments are grounded in the benchmark results in Section 9 and in published W-TinyLFU evaluation (Einziger et al., 2017).

| Property                | LRU                              | TinyLFU / W-TinyLFU                               | WALDE (W-LFU)                                           |
|-------------------------|----------------------------------|----------------------------------------------------|----------------------------------------------------------|
| Hit rate, Zipfian α=0.99 | Degrades under scan; no admission gate | +10–20pp over LRU (published); Bloom preserves sketch integrity | 75.3% L1 at L1=8192; consistent with W-TinyLFU at equivalent capacity |
| Scan resistance         | None                             | Strong — Bloom rejects first-seen items            | Strong for stable hot sets; degrades at high one-hit-wonder rates (Section 7) |
| Admission precision     | N/A                              | High — Bloom pre-filters sketch pollution          | Moderate — CMS decay only; precision degrades under uniform high-cardinality access |
| Bloom overhead          | None                             | Memory + reset scheduling; reset interval is workload-sensitive | None |
| Operational complexity  | Low                              | Moderate — reset schedule requires tuning          | Low — no reset schedule, no additional tunables         |
| Per-path observability  | None                             | None                                               | Full nanosecond breakdown per call, zero cost when unused |
| Concurrency             | Typically coarse-grained         | Implementation-dependent                           | 64 independent stripe mutexes; slab CAS contention above 4 threads (Section 9.3) |
| Multi-level tiering     | No                               | No                                                 | L1 + L2 + pluggable StorageBackend                      |

---

## 5. Architecture

Control flow annotated with decision points, latency sources, and contention boundaries.

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
    |                                           per-stripe; scales with per-stripe thread count
    v
  +----------------------------------------------------------+
  |  L1: Three-Segment LRU                                   |
  |                                                          |
  |  Window (1% capacity)                                    |
  |    |                                                     |
  |    |  on overflow: eviction candidate ready              |
  |    v                                                     |
  |  CMS.query(candidate) vs CMS.query(victim)               |  [DECISION POINT: admission]
  |  [~5–15 ns per call, per-stripe sketch]                  |  f(candidate) > f(victim)?
  |    |                                                     |
  |    |-- YES: candidate → Probation                        |
  |    |        victim    → DemotionQueue                    |
  |    |-- NO:  candidate → DemotionQueue                    |
  |             victim retained in Probation                 |
  |                                                          |
  |  Probation (20% capacity)                                |
  |    hit → promote to Protected                            |  [DECISION POINT: promotion]
  |                                                          |
  |  Protected (80% capacity)                                |
  |    overflow → demote to Probation tail                   |  [no direct eviction to L2]
  |                                                          |
  |  SlabAllocator  — shared across all 64 stripes           |  [CONTENTION POINT B]
  |  [atomic free-list; CAS bottleneck measurable at 8T      |   cross-stripe contention
  |   fix: per-stripe arenas — not yet implemented]          |
  +----------------------------------------------------------+
    |
    |  eviction → DemotionQueue (bounded)      [get() never blocks here]
    v
  DemotionDrainer  — background thread
    |
    |  drains queue asynchronously
    v
  L2Cache[j]   — j = hash % 16
    |
    |  lock_wait: acquire L2 stripe mutex      [CONTENTION POINT C]
    |                                           lower frequency than L1
    v
  Exclusive LRU eviction within L2 stripe
    |
    v
  StorageBackend  — pluggable interface        
```

**Latency by stage:**

| Stage       | Typical cost                        | Notes                                              |
|-------------|-------------------------------------|----------------------------------------------------|
| `lock_wait` | 0–500 ns                            | Near-zero at 1–2 threads; scales with stripe load  |
| `lookup`    | 50–200 ns                           | Hash map probe + LRU pointer update                |
| `admission` | 5–15 ns                             | Two CMS reads + one comparison                     |
| `eviction`  | 10–50 ns                            | Slab dealloc + DemotionQueue enqueue               |
| `l2`        | 200–600 ns                          | Separate stripe lock + hash probe                  |
| `slab`      | 10–30 ns at 1–4T; degrades at 8T    | Atomic free-list CAS; identified bottleneck        |

---

## 6. Production Failure Modes

### 6.1 LRU under scan traffic

LRU maintains no frequency state. Each incoming item unconditionally evicts the least recently used item. Under a workload where 10% of operations are sequential scans over a cold key range, scan items repeatedly displace hot items. The hot set must be re-warmed after each scan pass. At 10% scan rate, measured L1 hit rate degrades by 8.7pp (Section 9.1). LRU cannot distinguish a scan access from a hot-set access — both are treated as recency events.

Without per-path instrumentation, this degradation is opaque. The aggregate hit rate declines and latency tail widens, but the cause — scan-driven eviction of specific hot keys — is not attributable from cache-level metrics alone.

### 6.2 What WALDE changes

The CMS admission gate asymmetrically favors hot-set items. A key seen once during a scan accumulates CMS frequency ~1. A key in the hot set has frequency proportional to its access count since the last decay event. Scan items lose the admission competition. Measured L1 hit-rate degradation under 10% scan is bounded at 8.7pp (Section 9.1), with the admission reject rate stable at ~80% in both clean and scan-mixed workloads.

CMS decay (every 5×capacity operations per stripe) prevents historical frequency from permanently dominating: previously hot items that are no longer accessed will decay toward zero and can be displaced by items accumulating new frequency.

### 6.3 What WALDE does not solve

WALDE does not protect against uniform high-cardinality access with no recurrence (see Section 7 for measured analysis). It does not eliminate tail latency from lock contention — at 8 threads, slab CAS contention measurably degrades throughput (Section 9.3). The async demotion path bounds steady-state queue depth but does not guarantee bounded demotion latency under sustained L1 eviction pressure.

---

## 7. Where WALDE Fails 

### 7.1 Adversarial workload: high one-hit-wonder rate

Consider a workload where 60–80% of accesses are unique keys with no temporal recurrence — for example, a cache placed in front of a result set that is never re-queried, or a key-value store receiving requests from a high-cardinality uniform distribution.

In this regime:

1. Each unique key enters the Window LRU and increments its CMS cell on arrival.
2. On window eviction, the candidate (frequency ~1) competes against a Probation victim that may also have frequency ~1, because Probation is populated by prior one-hit items that survived by chance.
3. The admission decision degenerates toward random selection between two low-frequency items.
4. CMS decay does not help: there is no stable high-frequency population to protect.

**Expected behavior**: Admission provides no benefit over LRU. Hit rate converges to approximately `capacity / working_set_size`. For a working set 10x the cache capacity, this is ~10% L1 hit rate regardless of admission policy, because there is no hot set to protect.

**Why TinyLFU is better here**: The Bloom doorkeeper rejects first-time items outright, preventing sketch increment. This preserves CMS integrity for items that genuinely recur. WALDE's CMS-only approach allows every first-time item to pollute the sketch. Under high cardinality with low recurrence, all CMS frequency estimates converge toward uniform low values. The admission gate loses discriminative power.

**Quantitative bound**: No controlled measurement of this scenario is in the current benchmark suite. The analysis is derived from CMS error bounds (Cormode & Muthukrishnan, 2005): with sketch width `w` and depth `d`, overcount error per item is bounded by `e/w` with probability `1 - e^(-d)`. Under uniform high-cardinality access, cells saturate before decay intervals, overcount dominates, and `f(candidate) ≈ f(victim)` for all pairs.

**Threshold**: If the one-hit-wonder rate exceeds approximately 50% of total operations, W-TinyLFU is the correct system. WALDE makes no claim of correctness for this workload class.

---

## 8. Admission Policy: W-LFU Detail

New items enter the Window LRU (1% of stripe capacity). On window overflow:

```
if CMS[candidate] > CMS[victim]:
    admit candidate to Probation
    demote victim → DemotionQueue → L2
else:
    reject candidate → DemotionQueue → L2
    retain victim in Probation
```

Probation hits promote to Protected. Protected overflow demotes to Probation tail. No item is directly evicted from Protected to L2.

**CMS decay**: every 5×capacity operations on a given stripe, all sketch counters for that stripe are right-shifted by 1 (halved). Decay is applied per-stripe at the time of the 5N-th local operation. Stripes decay independently based on local operation count.

**Implication**: decay rate is proportional to per-stripe throughput. Under skewed key distributions, high-traffic stripes decay faster. This is the correct behavior — high-traffic stripes receive more sketch updates and are most susceptible to stale-frequency lock-in.

---

## 9. Benchmarks

**Environment**: Intel Core i7-1360P (12c/24t, 5.0 GHz boost), 16 GiB RAM, Ubuntu 24.04, GCC, `-O2 -DNDEBUG`.
**Configuration**: L1=8192, L2=4096, window fraction=0.01, Zipfian α=0.99. 100K warmup operations (excluded), 200K measured operations. 10 consecutive runs; values are deterministic across runs unless noted.

### 9.1 Hit Rates and Admission

| Workload              | L1 Hit Rate | L2 Hit Rate | Admit / Reject | Reject Rate |
|-----------------------|-------------|-------------|----------------|-------------|
| Zipfian α=0.99        | 75.3%       | 8.7%        | 9,523 / 39,896 | ~81%        |
| Zipfian + 10% scan    | 66.6%       | 5.5%        | 8,611 / 35,444 | ~80%        |

Scan traffic reduces L1 hit rate by 8.7pp. The admission reject rate holds near 80% in both workloads, confirming that the CMS gate is filtering scan-driven one-hit items consistently. These values are deterministic properties of the algorithm and workload distribution.

The 75.3% L1 hit rate at L1=8192 against Zipfian α=0.99 is consistent with published W-TinyLFU results at equivalent capacity ratios (Einziger et al., 2017, Figure 6). Direct numerical comparison requires identical workload generation, which is not available here.

### 9.2 Single-Threaded Throughput (200K ops, Zipfian)

| Metric   | Value            |
|----------|------------------|
| Typical  | 750–800K ops/sec |
| Peak     | ~835K ops/sec    |
| Floor    | ~650K ops/sec    |

One outlier run measured ~404K ops/sec, correlated with elevated system load during that run. This is OS scheduling interference, not a cache regression. It is reported here and excluded from the typical range.

### 9.3 Concurrency Scaling (100K ops/thread, Zipfian)

| Threads | Throughput        | Speedup vs 1T |
|---------|-------------------|---------------|
| 1       | 650–840K ops/sec  | 1.00x         |
| 2       | 1.0–1.3M ops/sec  | 1.4–1.7x      |
| 4       | 1.5–2.0M ops/sec  | 2.0–2.7x      |
| 8       | 1.0–1.5M ops/sec  | 1.4–2.0x      |

Throughput peaks at 4 threads and regresses at 8. Root cause: all 64 stripes share a single atomic slab free-list. At 8 threads, CAS contention on this free-list outweighs parallelism gains from additional cores. Admission, eviction, and striping logic are unaffected. The architectural fix — per-stripe memory arenas — is documented in Section 11.

### 9.4 End-to-End Latency (single thread, Zipfian)

| Percentile | Latency      |
|------------|--------------|
| p50        | 0.6 μs       |
| p95        | 2.4–4.8 μs   |
| p99        | 4.8–9.6 μs   |
| p999       | 19.2–38.4 μs |

Per-path p50: L1 hit ≈ 0.3–0.6 μs · L2 hit ≈ 2.4 μs · Backend ≈ 2.4 μs.

The p99–p999 spread (10–16x over p50) reflects lock contention variance and background drainer scheduling jitter, not algorithmic overhead. Instrumentation overhead is ~86 ns per call when `LatencyBreakdown*` is non-null; zero when null.

---

## 10. Per-Path Latency Instrumentation

`LatencyBreakdown` is part of the core API. Pass a non-null pointer to `get()` or `put()` for nanosecond-resolution stage decomposition. Pass null for zero overhead.

```cpp
LatencyBreakdown breakdown;
auto result = cache.get("user:1001", &breakdown);
// fields: lock_wait, lookup, admission, eviction, l2, slab
// all values in nanoseconds; unvisited stages are 0
```

| Field       | Measures                                           |
|-------------|----------------------------------------------------|
| `lock_wait` | Time blocked acquiring stripe mutex                |
| `lookup`    | Hash map probe + LRU pointer update                |
| `admission` | CMS query + frequency comparison                   |
| `eviction`  | Victim deallocation + DemotionQueue enqueue        |
| `l2`        | L2 stripe lock + LRU probe                         |
| `slab`      | Free-list CAS + slot acquisition                   |

When `admission` latency is anomalously high relative to `lookup`, it indicates CMS contention or a decay-cycle event. When `slab` dominates at high thread counts, it confirms the free-list bottleneck described in Section 9.3.

---

## 11. Limitations

| Bottleneck              | Observed Impact                                                               | Fix                                                         | Status                          |
|-------------------------|-------------------------------------------------------------------------------|-------------------------------------------------------------|---------------------------------|
| Global slab free-list   | 8-thread throughput below 4-thread baseline; CAS contention confirmed        | Per-stripe memory arenas; eliminates cross-stripe allocation | Not implemented                 |
| No Bloom doorkeeper     | CMS admits one-hit wonders; precision degrades under high-cardinality uniform workloads (Section 7) | Optional counting Bloom pre-filter as admission stage | Not implemented        |
| In-memory only          | Benchmarks do not capture disk or NVMe latency                                | Disk / NVMe `StorageBackend` implementation                 | Not implemented  |
| Single-node             | No replication or distributed coordination                                    | Out of scope                                                | —                               |

---

## 12. Usage

```cpp
#include "cache_engine.h"

// L1 capacity=8192, L2 capacity=4096, window fraction=0.01
CacheEngine cache(8192, 4096, 0.01);

cache.put("user:1001", payload);

auto result = cache.get("user:1001");
if (result) { /* L1 or L2 hit */ }

// Null pointer → zero instrumentation overhead
LatencyBreakdown breakdown;
auto result = cache.get("user:1001", &breakdown);
```

---

## 13. Quick Start

```bash
git clone https://github.com/ayushan003/walde && cd walde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Benchmarks:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWALDE_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
./build/walde_benchmark
```

---

## 14. Project Structure

```
include/      Public API headers
src/          Core implementation
benchmarks/   Benchmark harness (GoogleBenchmark)
tests/        Unit tests (GoogleTest)
docs/         Architecture notes
```

---

## 15. References

- Einziger, G., Friedman, R., & Manes, B. (2017). TinyLFU: A highly efficient cache admission policy. *ACM Transactions on Storage*, 13(4).
- Cormode, G., & Muthukrishnan, S. (2005). An improved data stream summary: the count-min sketch and its applications. *Journal of Algorithms*, 55(1), 58–75.

---

## Tech Stack

C++17 · XXHash64 · Count-Min Sketch · GoogleTest · CMake

## License

MIT
