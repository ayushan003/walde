#pragma once

#include "slab_allocator.h"
#include "types.h"

#include <cstdint>

namespace walde {

// ─── IntrusiveLRU ───────────────────────────────────────────
//
// Doubly-linked list stored in slab indices (not raw pointers).
// MRU = head, LRU = tail.
//
// All operations are O(1). No heap allocation beyond the slab.
// Not thread-safe — callers must hold the stripe mutex.

class IntrusiveLRU {
public:
    explicit IntrusiveLRU(SlabAllocator& slab);

    // Insert at MRU position (head).
    void push_front(uint32_t index);

    // Remove from any position. Caller must ensure index is in this list.
    void remove(uint32_t index);

    // Remove and return LRU (tail) element. Returns kInvalidIndex if empty.
    uint32_t pop_back();

    // Peek at MRU (head) without removing. Returns kInvalidIndex if empty.
    uint32_t peek_front() const { return head_; }

    // Peek at LRU (tail) without removing. Returns kInvalidIndex if empty.
    uint32_t peek_back() const { return tail_; }

    // Move an existing element to MRU position.
    void move_to_front(uint32_t index);

    uint32_t size()  const { return size_; }
    bool     empty() const { return size_ == 0; }

private:
    SlabAllocator& slab_;
    uint32_t head_ = kInvalidIndex;
    uint32_t tail_ = kInvalidIndex;
    uint32_t size_ = 0;
};

}  // namespace walde
