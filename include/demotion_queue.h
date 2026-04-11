#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace walde {

struct DemotionItem {
    std::string key;
    std::string value;
    uint32_t    frequency = 0;
};

class DemotionQueue {
public:
    explicit DemotionQueue(uint32_t capacity);

    bool try_push(DemotionItem item);
    bool try_pop(DemotionItem& out);
    size_t try_pop_batch(std::vector<DemotionItem>& out, size_t max_count);

    uint32_t size()             const;
    uint32_t capacity()         const { return capacity_; }
    bool     is_under_pressure() const;
    uint64_t drops()            const {
        std::lock_guard<std::mutex> lk(mtx_);
        return drops_;
    }

private:
    uint64_t unlocked_size() const {
        return write_pos_ - read_pos_;
    }
    static uint32_t next_power_of_2(uint32_t v);

    const uint32_t   capacity_;
    const uint32_t   mask_;
    mutable std::mutex mtx_;
    std::vector<DemotionItem> buffer_;
    uint64_t write_pos_ = 0;
    uint64_t read_pos_  = 0;
    uint64_t drops_     = 0;

    static constexpr float pressure_threshold_ = 0.80f;
};

}  // namespace walde
