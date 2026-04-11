#pragma once

#include "slab_allocator.h"
#include "types.h"

#include <cstdint>

namespace walde {

class IntrusiveLRU {
public:
    explicit IntrusiveLRU(SlabAllocator& slab);

    void push_front(uint32_t index);
    void remove(uint32_t index);
    uint32_t pop_back();
    uint32_t peek_front() const { return head_; }
    uint32_t peek_back() const { return tail_; }
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
