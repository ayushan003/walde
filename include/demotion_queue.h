#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace walde {

// ─── DemotionItem ───────────────────────────────────────────
// Data carried from L1 eviction to L2 insertion.
struct DemotionItem {
    std::string key;
    std::string value;
    uint32_t    frequency = 0;  // CMS estimate at eviction time
};

// ─── DemotionQueue ──────────────────────────────────────────
//
// Bounded MPSC ring buffer that decouples L1 eviction from L2 insertion.
// L1 stripes push evicted items here (producers). The DemotionDrainer
// thread drains in batches into L2 (consumer).
//
// Backpressure: drops items when the queue exceeds 80% capacity
// rather than blocking L1 writers. Dropped items are simply lost
// (not inserted into L2). This is acceptable: L2 is a best-effort
// cache layer, not a durability guarantee.
//
// Thread safety: all operations are mutex-guarded.

class DemotionQueue {
public:
    explicit DemotionQueue(uint32_t capacity);

    // Push one item. Returns false and drops if queue is under pressure.
    bool try_push(DemotionItem item);

    // Pop one item. Returns false if empty.
    bool try_pop(DemotionItem& out);

    // Pop up to max_count items into out. Returns number actually popped.
    size_t try_pop_batch(std::vector<DemotionItem>& out, size_t max_count);

    uint32_t size()             const;
    uint32_t capacity()         const { return capacity_; }
    bool     is_under_pressure() const;
    uint64_t drops()            const {
        std::lock_guard<std::mutex> lk(mtx_);
        return drops_;
    }

private:
    uint32_t unlocked_size() const {
        return write_pos_ - read_pos_;
    }
    static uint32_t next_power_of_2(uint32_t v);

    const uint32_t   capacity_;
    const uint32_t   mask_;
    mutable std::mutex mtx_;
    std::vector<DemotionItem> buffer_;
    uint32_t write_pos_ = 0;
    uint32_t read_pos_  = 0;
    uint64_t drops_     = 0;

    static constexpr float pressure_threshold_ = 0.80f;
};

}  // namespace walde
