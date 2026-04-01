# WALDE — W-LFU Multi-Level Cache Engine

A multi-level caching library in C++17 implementing the W-LFU (Window
Least Frequently Used) admission policy with per-stripe concurrency,
nanosecond-precision latency instrumentation, and a reproducible
benchmark suite.

## Architecture

```
  Client ──────────────▶  CacheEngine
                              │
                     XXHash64(key) % 64
                              │
                    ┌─────────▼──────────┐
                    │    CacheStripe     │  ×64 (independent mutexes)
                    │  ┌─────────────┐   │
                    │  │ Window LRU  │ 1%│
                    │  │ Probation   │20%│  main = 99% capacity
                    │  │ Protected   │80%│
                    │  └─────────────┘   │
                    │  Count-Min Sketch  │
                    │  Slab Allocator    │
                    └─────────┬──────────┘
                              │ eviction
                    ┌─────────▼──────────┐
                    │  DemotionQueue     │  bounded queue
                    │  DemotionDrainer   │  background thread
                    └─────────┬──────────┘
                              │
                    ┌─────────▼──────────┐
                    │  L2 Cache [0..15]  │  16 stripes, exclusive LRU
                    └─────────┬──────────┘
                              │
                    ┌─────────▼──────────┐
                    │  StorageBackend    │  pluggable interface
                    └────────────────────┘
```

## Admission Policy (W-LFU)

New items always enter the **Window LRU** (1% of per-stripe capacity).
On window overflow the LRU candidate competes against the LRU tail of
**Probation** using Count-Min Sketch frequency estimates:

- `CMS[candidate] > CMS[victim]`: candidate admitted, victim evicted to L2.
- Otherwise: candidate rejected and freed.

Items hit while in Probation are promoted to **Protected**. Protected
overflow demotes back to Probation (never directly evicted).

This is W-LFU, not W-TinyLFU: there is no Bloom-filter doorkeeper.
One-hit wonders increment the CMS; CMS decay (halving every 5×capacity ops)
mitigates stale frequency pollution.

## Build

```bash
git clone https://github.com/ayushan003/walde && cd walde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Run Benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWALDE_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
./build/walde_benchmark
```

## Test Environment

| Component | Details |
|-----------|---------|
| CPU | Intel Core i7-1360P (13th Gen), 1 socket, 12 cores, 24 threads |
| Max clock | 5.0 GHz |
| L1d / L1i | 448 KiB / 640 KiB (12 instances each) |
| L2 | 9 MiB (6 instances) |
| L3 | 18 MiB (1 instance, shared) |
| RAM | 16 GiB (10 GiB available during runs) |
| OS | Ubuntu 24.04.4 LTS, kernel 6.17.0-19-generic |
| Build | GCC, `-DCMAKE_BUILD_TYPE=Release` |

## Benchmark Results

Measured across 10 consecutive runs on the machine above. Configuration:
L1=8192, L2=4096, α=0.99, 100K warmup + 200K measured ops. Throughput
varies with system load; run `walde_benchmark` for numbers on your hardware.

### Hit Rates and Admission (deterministic — identical across all 10 runs)

These values are determined entirely by the workload and cache configuration,
not by timing:

| Workload            | L1 Hit Rate | L2 Hit Rate | Admissions | Rejections |
|---------------------|-------------|-------------|------------|------------|
| Zipfian α=0.99      | 75.3%       | 8.7%        | 9,523      | 39,896     |
| Zipfian + 10% scan  | 66.6%       | 5.5%        | 8,611      | 35,444     |

Scan traffic costs ~8.7pp of L1 hit rate. The rejection ratio holds at
~4:1 in both workloads, showing the admission gate consistently filters
scan-driven one-hit items.

### Throughput (varies with system load)

Single-threaded, 200K ops, Zipfian α=0.99:

| Metric  | Value             |
|---------|-------------------|
| Minimum | ~650K ops/sec     |
| Typical | ~750–800K ops/sec |
| Maximum | ~835K ops/sec     |

One run recorded ~404K ops/sec coinciding with elevated system load
(that run's timer overhead was also the highest of all 10). Under
consistent idle-machine conditions the floor is ~650K.

### Concurrency Scaling (Zipfian, 100K ops/thread)

| Threads | Typical throughput | Typical speedup |
|---------|--------------------|-----------------|
| 1       | 650–840K ops/sec   | 1.00×           |
| 2       | 1.0–1.3M ops/sec   | ~1.4–1.7×       |
| 4       | 1.5–2.0M ops/sec   | ~2.0–2.7×       |
| 8       | 1.0–1.5M ops/sec   | ~1.4–2.0×       |

**8-thread throughput is lower than 4-thread in every single run.**
This is a structural regression, not noise. Root cause: all 64 stripes
share a single atomic slab free-list; CAS contention at 8 threads
exceeds the parallelism gain from additional cores. See Known Limitations.

### End-to-End Latency (single thread, Zipfian)

| Percentile | Observed range |
|------------|----------------|
| p50        | 0.6 μs         |
| p95        | 2.4–4.8 μs     |
| p99        | 4.8–9.6 μs     |
| p999       | 19.2–38.4 μs   |

Per-path p50: L1 hit ≈ 0.3–0.6 μs · L2 hit ≈ 2.4 μs · Backend ≈ 2.4 μs

### Timer Overhead

`steady_clock` overhead across 10 runs: 34–49 ns, median 43 ns.
Per-operation instrumentation cost: ~86 ns (two clock reads).

## Per-Path Latency Instrumentation

Every `get()` / `put()` call accepts an optional `LatencyBreakdown*`.
When provided, each pipeline stage writes its nanosecond duration into
the struct. The benchmark harness accumulates these into per-path
fixed-bucket histograms.

| Component   | Measures                                     |
|-------------|----------------------------------------------|
| `lock_wait` | Blocked time waiting for stripe mutex        |
| `lookup`    | Hash map find + LRU list promotion           |
| `admission` | CMS estimate + frequency comparison          |
| `eviction`  | Victim demotion + slab slot dealloc          |
| `l2`        | L2 stripe lookup                             |
| `backend`   | Storage backend access                       |
| `slab`      | Slab slot allocation                         |

## Project Structure

```
include/          Core library headers (flattened)
src/              Core library implementation
benchmarks/       Benchmark harness (stdout only)
tests/            GoogleTest unit tests
docs/             Architecture notes
```

## Known Limitations

1. **Slab CAS contention at >4 threads.** All 64 stripes share a single
   atomic free-list. 8-thread throughput regresses below 4-thread in every
   measured run. Fix: per-stripe memory arenas.

2. **No Bloom doorkeeper.** One-hit wonders increment the CMS. CMS decay
   mitigates but does not eliminate frequency pollution. Fix: counting Bloom
   pre-filter.

3. **InMemoryBackend in benchmarks.** Results reflect cache behavior only;
   real disk I/O latency is not captured.

4. **Single-node only.** No distributed coordination.

## Tech Stack

C++17 · XXHash64 · GoogleTest · CMake · Count-Min Sketch · W-LFU

## License

MIT
