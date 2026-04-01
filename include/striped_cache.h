#pragma once

#include "cache_stripe.h"
#include "demotion_queue.h"
#include "latency_instrumentation.h"
#include "slab_allocator.h"
#include "types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace walde {

// ─── StripedCache ───────────────────────────────────────────
//
// 64-way striped L1 cache. Routes keys via XXH64 to reduce lock
// contention under concurrent load (each stripe has its own mutex).
//
// All 64 stripes share a single SlabAllocator for memory efficiency.
// Shared slab CAS contention becomes the bottleneck above ~4 threads;
// see architecture notes on per-stripe arenas as the future fix.

class StripedCache {
public:
    struct Config {
        uint32_t total_capacity = 8192;
        float    window_pct     = 0.01f;
        float    probation_pct  = 0.20f;
    };

    explicit StripedCache(const Config& config);

    std::optional<std::string> get(const std::string& key,
                                   LatencyBreakdown*  bd = nullptr);
    bool put(const std::string& key, const std::string& value,
             LatencyBreakdown*  bd = nullptr);
    bool remove(const std::string& key);

    // Batch get (sequential; no cross-stripe optimization).
    std::vector<std::optional<std::string>> batch_get(
        const std::vector<std::string>& keys);

    void set_demotion_queue(DemotionQueue* q) {
        for (auto& s : stripes_) s->set_demotion_queue(q);
    }

    // Aggregate stats (atomic reads, no lock needed).
    uint64_t total_hits()       const;
    uint64_t total_misses()     const;
    uint64_t total_evictions()  const;
    uint64_t total_admissions() const;
    uint64_t total_rejections() const;
    uint32_t total_size()       const;

    uint64_t stripe_hits(uint32_t stripe_id)   const;
    uint64_t stripe_misses(uint32_t stripe_id) const;

private:
    uint32_t route(const std::string& key) const;

    SlabAllocator                                 slab_;
    std::array<std::unique_ptr<CacheStripe>, kNumStripes> stripes_;
};

}  // namespace walde
