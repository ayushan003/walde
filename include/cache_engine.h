#pragma once

#include "demotion_drainer.h"
#include "demotion_queue.h"
#include "latency_instrumentation.h"
#include "storage_backend.h"
#include "striped_cache.h"
#include "types.h"

#include <array>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace walde {

// ─── CacheEngine ────────────────────────────────────────────
//
// Top-level cache with three tiers:
//   L1 (StripedCache):  64-way striped W-LFU, nanosecond latency.
//   L2 (16-way SLRU):   Absorbs L1 evictions, microsecond latency.
//   Backend:            Persistent store, arbitrary latency.
//
// Read path: L1 → L2 (exclusive: remove on hit) → Backend.
// Write path: Backend always written; L1 updated.
//
// L2 is populated asynchronously by DemotionDrainer from L1 evictions.
// This keeps the L1 eviction path off the critical latency path.

class CacheEngine {
public:
    struct Config {
        StripedCache::Config l1_config;
        uint32_t             l2_capacity          = 4096;
        uint32_t             demotion_queue_size   = 2048;
    };

    explicit CacheEngine(const Config& config,
                         std::unique_ptr<StorageBackend> backend);
    ~CacheEngine();

    std::optional<std::string> get(const std::string& key,
                                   LatencyBreakdown*  bd = nullptr);
    bool   put(const std::string& key, const std::string& value,
               LatencyBreakdown* bd = nullptr);
    bool   remove(const std::string& key);

    // ── Stats ──────────────────────────────────────────────
    uint64_t l1_hits()       const { return l1_.total_hits(); }
    uint64_t l1_misses()     const { return l1_.total_misses(); }
    uint64_t l1_evictions()  const { return l1_.total_evictions(); }
    uint64_t l1_admissions() const { return l1_.total_admissions(); }
    uint64_t l1_rejections() const { return l1_.total_rejections(); }
    uint32_t l1_size()       const { return l1_.total_size(); }

    uint64_t l2_hits()   const;
    uint64_t l2_misses() const;
    uint32_t l2_size()   const;

    uint64_t backend_reads()  const { return backend_->reads(); }
    uint64_t backend_writes() const { return backend_->writes(); }

    double l1_hit_rate()      const;
    double overall_hit_rate() const;

private:
    // ── L2 internals ───────────────────────────────────────
    static constexpr size_t kNumL2Stripes = 16;

    struct L2Node {
        std::string key;
        std::string value;
    };

    struct L2Stripe {
        mutable std::mutex                   mutex;
        std::list<L2Node>                    list;   // MRU at front
        std::unordered_map<std::string,
            std::list<L2Node>::iterator>     map;
        std::atomic<uint64_t>                hits   {0};
        std::atomic<uint64_t>                misses {0};
    };

    size_t                     get_l2_stripe_index(const std::string& key) const;
    std::optional<std::string> l2_get(const std::string& key, int64_t* l2_ns);
    void                       l2_put_batch(const std::vector<DemotionItem>& batch);

    // ── Members ────────────────────────────────────────────
    StripedCache                              l1_;
    std::unique_ptr<StorageBackend>           backend_;
    DemotionQueue                             demotion_queue_;
    std::array<std::unique_ptr<L2Stripe>,
               kNumL2Stripes>                 l2_stripes_;
    uint32_t                                  l2_capacity_per_stripe_;
    std::unique_ptr<DemotionDrainer>          drainer_;
};

}  // namespace walde
