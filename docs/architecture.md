# WALDE — Architecture Notes

## Request Pipeline

```
put(key, value):
  → XXHash64(key) % 64 → stripe_id
  → lock(stripe mutex)
  → CMS.increment(key)
  → maybe_decay_cms()        // halve counters every 5×capacity ops
  → if key exists:
      update value, promote within segment
      unlock, return
  → slab.allocate()          // lock-free CAS, O(1)
      if exhausted → return false
  → insert into window LRU (MRU position)
  → while window.size > window_capacity:
      candidate = window.pop_LRU()
      if main has room:
          admit candidate to probation directly
      elif probation is empty:
          reject candidate, free slab slot
      else:
          victim = probation.peek_LRU()
          if CMS[candidate] > CMS[victim]:
              evict victim → demotion queue → L2
              admit candidate to probation
          else:
              reject candidate, free slab slot
  → unlock, return

get(key):
  → route to stripe, lock
  → index_map.find(key)
  → if miss: increment miss counter, unlock, return nullopt
  → if hit:
      CMS.increment(key)
      promote within segment:
        window    → move to MRU
        probation → promote to protected
        protected → move to MRU
      unlock, return value
  → (at engine level) on L1 miss:
      check L2 (16-way striped, exclusive) → if hit, remove from L2, insert to L1
      check backend → if hit, insert to L1
```

## Invariants

1. CMS increments on every put() and on get() hits only
2. New items always enter window first
3. Admission to main only via window overflow
4. Frequency comparison: candidate must STRICTLY beat probation LRU victim
5. Protected items are never evicted for admission — only probation victims
6. Evicted victims flow to L2 via async bounded demotion queue
7. No global locks — all L1 state is per-stripe
8. L1 and L2 are exclusive (no duplicates)
9. Slab has headroom of kNumStripes slots for temporary window overshoot

## Capacity Math (per stripe, total_capacity=8192)

```
per_stripe = 8192 / 64 = 128
window     = max(3, 128 × 0.01) = 3
main       = 128 - 3 = 125
probation  = max(1, 125 × 0.20) = 25
protected  = 125 - 25 = 100
slab total = 8192 + 64 = 8256
```

## CMS Configuration

- Depth: 4 rows
- Width: 8192 counters per row
- Decay: halve all counters every 5×capacity stripe ops
- Decay trigger: separate `ops_since_decay_` counter (not modulo of
  total_increments, which would spuriously trigger at op 0)
- Hit-only incrementing on get(): prevents scan keys from building
  false frequency on read-only workloads

## Why W-LFU, not W-TinyLFU

TinyLFU = doorkeeper Bloom filter + Count-Min Sketch. The doorkeeper
rejects items that haven't been seen before (one-hit wonders), preventing
them from ever incrementing the CMS.

This implementation omits the doorkeeper. Items that appear exactly once
still increment the CMS, slightly inflating frequency estimates for other
items in the same CMS bucket. CMS decay reduces this effect over time.

## Stat Counter Thread Safety

All per-stripe stat counters (`hits_`, `misses_`, `evictions_`,
`admissions_`, `rejections_`) are `std::atomic<uint64_t>`.
They are written under the stripe mutex (only one writer possible),
but read without the mutex by `StripedCache::total_hits()` etc.
Using atomics prevents the data race that would otherwise exist on
non-atomic reads outside the lock.

## Slab Contention

The slab allocator uses a single `std::atomic<uint64_t>` head with tagged
CAS. All 64 stripes share one slab. At >4 threads, CAS retry rate increases
because stripes contend on the same cache line.

Measured degradation: ~2.6× throughput at 8 threads vs 1 thread (see
benchmark results). Root cause: shared slab, not stripe-level contention.

Fix (not implemented): per-stripe memory arenas with thread-local bump
allocators and a global overflow pool.

## L2 Architecture

- 16-way striped (independent from L1's 64 stripes)
- Per-stripe: `std::list<L2Node>` + `unordered_map` for O(1) LRU
- Exclusive caching: items removed from L2 on hit
- Fed by DemotionDrainer (background thread, batch ≤64 items/cycle)
- Backpressure: demotion queue drops items when >80% full
- Default capacity: 4096 (half of L1 default of 8192)

## Latency Instrumentation

Every `get()` and `put()` call optionally accepts a `LatencyBreakdown*`.
When non-null, each pipeline stage writes its elapsed nanoseconds into the
struct. The benchmark harness passes a breakdown per operation and
accumulates results into per-path histograms (L1 hit, L2 hit, backend hit).

Component fields: `lock_wait_ns`, `lookup_ns`, `admission_ns`,
`eviction_ns`, `l2_ns`, `backend_ns`, `slab_ns`.
