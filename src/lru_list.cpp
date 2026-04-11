#include "lru_list.h"

#include <cassert>

namespace walde {

IntrusiveLRU::IntrusiveLRU(SlabAllocator& slab)
    : slab_(slab)
{}

void IntrusiveLRU::push_front(uint32_t index) {
    assert(index != kInvalidIndex);
    CacheNode& node = slab_.node(index);
    node.prev = kInvalidIndex;
    node.next = head_;
    if (head_ != kInvalidIndex) {
        slab_.node(head_).prev = index;
    } else {
        tail_ = index;
    }
    head_ = index;
    ++size_;
}

void IntrusiveLRU::remove(uint32_t index) {
    assert(index != kInvalidIndex);
    assert(size_ > 0 && "remove from empty list");
    CacheNode& node = slab_.node(index);
    if (node.prev != kInvalidIndex) {
        slab_.node(node.prev).next = node.next;
    } else {
        head_ = node.next;
    }
    if (node.next != kInvalidIndex) {
        slab_.node(node.next).prev = node.prev;
    } else {
        tail_ = node.prev;
    }
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
    if (index == head_) {
        return;
    }
    remove(index);
    push_front(index);
}

}  // namespace walde
