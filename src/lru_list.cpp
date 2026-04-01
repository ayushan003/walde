#include "lru_list.h"

#include <cassert>

namespace walde {

IntrusiveLRU::IntrusiveLRU(SlabAllocator& slab)
    : slab_(slab)
{}

void IntrusiveLRU::push_front(uint32_t index) {
    assert(index != kInvalidIndex);

    CacheNode& node = slab_.node(index);

    // New node has no predecessor, its successor is current head
    node.prev = kInvalidIndex;
    node.next = head_;

    if (head_ != kInvalidIndex) {
        // Old head's predecessor is now this node
        slab_.node(head_).prev = index;
    } else {
        // List was empty — this node is also the tail
        tail_ = index;
    }

    head_ = index;
    ++size_;
}

void IntrusiveLRU::remove(uint32_t index) {
    assert(index != kInvalidIndex);
    assert(size_ > 0 && "remove from empty list");

    CacheNode& node = slab_.node(index);

    // Stitch predecessor to successor
    if (node.prev != kInvalidIndex) {
        slab_.node(node.prev).next = node.next;
    } else {
        // Removing the head
        head_ = node.next;
    }

    // Stitch successor to predecessor
    if (node.next != kInvalidIndex) {
        slab_.node(node.next).prev = node.prev;
    } else {
        // Removing the tail
        tail_ = node.prev;
    }

    // Clear linkage on the removed node
    node.prev = kInvalidIndex;
    node.next = kInvalidIndex;

    --size_;
}

uint32_t IntrusiveLRU::pop_back() {
    if (tail_ == kInvalidIndex) {
        return kInvalidIndex;
    }

    uint32_t victim = tail_;
    remove(victim);
    return victim;
}

void IntrusiveLRU::move_to_front(uint32_t index) {
    assert(index != kInvalidIndex);

    // Already at front — no work needed
    if (index == head_) {
        return;
    }

    // Remove from current position and re-insert at front.
    // This is safe because remove() clears prev/next on the node,
    // and push_front() sets them fresh.
    remove(index);
    push_front(index);
}

}  // namespace walde
