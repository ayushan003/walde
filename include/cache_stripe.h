#pragma once

#include "count_min_sketch.h"
#include "demotion_queue.h"
#include "latency_instrumentation.h"
#include "lru_list.h"
#include "slab_allocator.h"
#include "types.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace walde {

// ─── CacheStripe ────────────────────────────────────────────
//
// One shard of the L1 cache. Implements the W-LFU policy:
//   Window LRU (1%):     New items enter here first.
//   Main SLRU:
//     Probation (20%):   Items promoted from window on admission.
//     Protected (80%):   Items promoted from probation on re-access.
//
// Admission gate (window → probation):
//   When window overflows, the LRU candidate is admitted to probation
//   only if its CMS frequency STRICTLY exceeds the probation LRU victim.
//   Otherwise the candidate is rejected and freed. Evicted victims
//   flow to L2 via the DemotionQueue.
//
// Thread safety:
//   All public methods acquire stripe mutex_.
//   Stat counters (hits_, misses_, etc.) are std::atomic to allow
//   lock-free reads from external stats collectors.

class CacheStripe {
public:
    CacheStripe(SlabAllocator& slab, uint32_t capacity,
                float window_pct    = 0.01f,
                float probation_pct = 0.20f);

    CacheStripe(const CacheStripe&)            = delete;
    CacheStripe& operator=(const CacheStripe&) = delete;

    void set_demotion_queue(DemotionQueue* queue) { demotion_queue_ = queue; }

    // bd is optional — when non-null, per-phase timing is recorded.
    std::optional<std::string> get(const std::string& key,
                                   LatencyBreakdown*  bd = nullptr);
    bool put(const std::string& key, const std::string& value,
             LatencyBreakdown*  bd = nullptr);
    bool remove(const std::string& key);

    uint32_t size()     const;
    uint32_t capacity() const { return total_capacity_; }

    // FIX: Atomic reads — no lock required, safe from any thread.
    uint64_t hit_count()       const { return hits_.load(std::memory_order_relaxed); }
    uint64_t miss_count()      const { return misses_.load(std::memory_order_relaxed); }
    uint64_t eviction_count()  const { return evictions_.load(std::memory_order_relaxed); }
    uint64_t admission_count() const { return admissions_.load(std::memory_order_relaxed); }
    uint64_t rejection_count() const { return rejections_.load(std::memory_order_relaxed); }

private:
    void insert_into_window(uint32_t idx, LatencyBreakdown* bd);
    void try_admit_from_window(LatencyBreakdown* bd);
    void promote_to_protected(uint32_t idx);
    void demote_victim(uint32_t victim_idx);
    void maybe_decay_cms();

    SlabAllocator& slab_;
    uint32_t total_capacity_;
    uint32_t window_capacity_;
    uint32_t main_capacity_;
    uint32_t probation_capacity_;
    uint32_t protected_capacity_;

    mutable std::mutex                        mutex_;
    std::unordered_map<std::string, uint32_t> index_map_;

    IntrusiveLRU window_;
    IntrusiveLRU probation_;
    IntrusiveLRU protected_;

    CountMinSketch cms_;
    uint64_t       decay_interval_;
    uint64_t       ops_since_decay_ = 0;  // FIX: separate counter avoids modulo-0 bug

    // FIX: atomic counters — written under mutex, read without lock by stat aggregators.
    std::atomic<uint64_t> hits_       {0};
    std::atomic<uint64_t> misses_     {0};
    std::atomic<uint64_t> evictions_  {0};
    std::atomic<uint64_t> admissions_ {0};
    std::atomic<uint64_t> rejections_ {0};

    DemotionQueue* demotion_queue_ = nullptr;
};

}  // namespace walde
