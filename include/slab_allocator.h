#pragma once

#include "types.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace walde {

// ─── SlabAllocator ──────────────────────────────────────────
//
// Pre-allocates a contiguous array of CacheNode structs at startup.
// Runtime allocation/deallocation is O(1) via a lock-free free-list
// stack using tagged CAS to prevent the ABA problem.
//
// ABA prevention:
//   The 64-bit head_ packs (index:32 | tag:32). Tag increments on
//   every push (deallocate). On pop (allocate), the CAS uses the
//   current tag — if another thread pushed the same index back,
//   the tag will differ and the CAS fails, forcing a retry.
//   The free_next_ read before CAS is safe: if CAS succeeds, head_
//   has not changed since the read, so free_next_[old_index] is
//   still valid (no concurrent writer could have modified it while
//   head_ pointed at old_index with the same tag).
//
// Thread safety:
//   allocate() and deallocate() are lock-free (CAS loop).
//   node() is safe if the index came from allocate().
//   available() may be transiently stale (relaxed load).

class SlabAllocator {
public:
    explicit SlabAllocator(uint32_t capacity);

    SlabAllocator(const SlabAllocator&)            = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;
    SlabAllocator(SlabAllocator&&)                 = delete;
    SlabAllocator& operator=(SlabAllocator&&)      = delete;

    // Returns an index to a free CacheNode, or kInvalidIndex if
    // the pool is exhausted. O(1) amortized, lock-free.
    uint32_t allocate();

    // Returns a CacheNode (by index) to the free pool. O(1), lock-free.
    void deallocate(uint32_t index);

    CacheNode&       node(uint32_t index);
    const CacheNode& node(uint32_t index) const;

    uint32_t capacity()  const { return capacity_; }
    uint32_t available() const {
        return free_count_.load(std::memory_order_relaxed);
    }

private:
    static uint64_t pack(uint32_t index, uint32_t tag) {
        return (static_cast<uint64_t>(tag) << 32) | index;
    }
    static uint32_t unpack_index(uint64_t packed) {
        return static_cast<uint32_t>(packed & 0xFFFFFFFF);
    }
    static uint32_t unpack_tag(uint64_t packed) {
        return static_cast<uint32_t>(packed >> 32);
    }

    uint32_t               capacity_;
    std::vector<CacheNode> pool_;       // Contiguous node storage
    std::vector<uint32_t>  free_next_;  // Free-list linkage (separate from LRU next)
    std::atomic<uint64_t>  head_;       // Tagged pointer: index | (tag << 32)
    std::atomic<uint32_t>  free_count_; // Approximate free slot count
};

}  // namespace walde
