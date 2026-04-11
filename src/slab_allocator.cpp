#include "slab_allocator.h"

#include <cassert>
#include <stdexcept>

namespace walde {

SlabAllocator::SlabAllocator(uint32_t capacity)
    : capacity_(capacity)
    , pool_(capacity)
    , free_next_(capacity)
    , head_(pack(0, 0))
    , free_count_(capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("SlabAllocator capacity must be > 0");
    }
    for (uint32_t i = 0; i < capacity - 1; ++i) {
        free_next_[i] = i + 1;
    }
    free_next_[capacity - 1] = kInvalidIndex;
}

uint32_t SlabAllocator::allocate() {
    uint64_t old_packed = head_.load(std::memory_order_acquire);
    while (true) {
        uint32_t old_index = unpack_index(old_packed);
        uint32_t old_tag   = unpack_tag(old_packed);
        if (old_index == kInvalidIndex) {
            return kInvalidIndex;
        }
        uint32_t new_index = free_next_[old_index];
        // Tag must increment on BOTH allocate and deallocate to
        // prevent ABA: without this, two allocations at the same
        // tag value could silently corrupt the free list.
        uint64_t new_packed = pack(new_index, old_tag + 1);
        if (head_.compare_exchange_weak(
                old_packed, new_packed,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            pool_[old_index].reset();
            free_count_.fetch_sub(1, std::memory_order_relaxed);
            return old_index;
        }
    }
}

void SlabAllocator::deallocate(uint32_t index) {
    assert(index < capacity_ && "deallocate: index out of bounds");
    pool_[index].reset();
    uint64_t old_packed = head_.load(std::memory_order_relaxed);
    while (true) {
        uint32_t old_index = unpack_index(old_packed);
        uint32_t old_tag   = unpack_tag(old_packed);
        free_next_[index] = old_index;
        uint64_t new_packed = pack(index, old_tag + 1);
        if (head_.compare_exchange_weak(
                old_packed, new_packed,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            break;
        }
    }
    free_count_.fetch_add(1, std::memory_order_relaxed);
}

CacheNode& SlabAllocator::node(uint32_t index) {
    assert(index < capacity_ && "node: index out of bounds");
    return pool_[index];
}

const CacheNode& SlabAllocator::node(uint32_t index) const {
    assert(index < capacity_ && "node: index out of bounds");
    return pool_[index];
}

}  // namespace walde
