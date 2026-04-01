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

    // Thread the free list through the separate free_next_ array.
    // Node 0 → 1 → 2 → ... → (capacity-1) → kInvalidIndex
    for (uint32_t i = 0; i < capacity - 1; ++i) {
        free_next_[i] = i + 1;
    }
    free_next_[capacity - 1] = kInvalidIndex;
}

uint32_t SlabAllocator::allocate() {
    // Lock-free pop with tagged pointer to prevent ABA.
    //
    // We read the packed head (index + tag). If the index is valid,
    // we read free_next_[index] to find the new head, and CAS with
    // the SAME tag (tag only changes on push/deallocate).
    //
    // Why tagged CAS prevents ABA:
    //   Thread A reads head = {index=5, tag=10}, reads free_next_[5]=7.
    //   Thread B pops 5, pops 7, pushes 5 back → head = {5, tag=12}.
    //   Thread A's CAS compares against {5, tag=10} — fails because
    //   tag is now 12. Thread A retries with fresh state. Correct.
    uint64_t old_packed = head_.load(std::memory_order_acquire);

    while (true) {
        uint32_t old_index = unpack_index(old_packed);
        uint32_t old_tag   = unpack_tag(old_packed);

        if (old_index == kInvalidIndex) {
            return kInvalidIndex;  // Pool exhausted
        }

        uint32_t new_index = free_next_[old_index];
        uint64_t new_packed = pack(new_index, old_tag);

        if (head_.compare_exchange_weak(
                old_packed, new_packed,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            // Successfully popped. Clean the node for use.
            pool_[old_index].reset();
            free_count_.fetch_sub(1, std::memory_order_relaxed);
            return old_index;
        }
        // CAS failed — old_packed was updated, retry automatically.
    }
}

void SlabAllocator::deallocate(uint32_t index) {
    assert(index < capacity_ && "deallocate: index out of bounds");

    // Reset the node so the next consumer gets a clean slate.
    pool_[index].reset();

    // Lock-free push with tag increment.
    // Every push bumps the tag, so even if the same index returns
    // to the head position, the tag is different → no ABA.
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
        // CAS failed — old_packed updated, retry.
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
