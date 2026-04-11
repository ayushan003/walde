#pragma once

#include "types.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace walde {

class SlabAllocator {
public:
    explicit SlabAllocator(uint32_t capacity);

    SlabAllocator(const SlabAllocator&)            = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;
    SlabAllocator(SlabAllocator&&)                 = delete;
    SlabAllocator& operator=(SlabAllocator&&)      = delete;

    uint32_t allocate();
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
    std::vector<CacheNode> pool_;
    std::vector<uint32_t>  free_next_;
    std::atomic<uint64_t>  head_;
    std::atomic<uint32_t>  free_count_;
};

}  // namespace walde
