#include "demotion_queue.h"
#include <algorithm>

namespace walde {

DemotionQueue::DemotionQueue(uint32_t capacity)
    : capacity_(next_power_of_2(capacity))
    , mask_(capacity_ - 1)
    , buffer_(capacity_)
{}

bool DemotionQueue::try_push(DemotionItem item) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (unlocked_size() >= static_cast<uint32_t>(capacity_ * pressure_threshold_)) {
        drops_++;
        return false;
    }

    if (write_pos_ - read_pos_ >= capacity_) {
        drops_++;
        return false;
    }

    buffer_[write_pos_ & mask_] = std::move(item);
    write_pos_++;
    return true;
}

bool DemotionQueue::try_pop(DemotionItem& out) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (read_pos_ == write_pos_) {
        return false;
    }

    out = std::move(buffer_[read_pos_ & mask_]);
    read_pos_++;
    return true;
}

size_t DemotionQueue::try_pop_batch(std::vector<DemotionItem>& out, size_t max_count) {
    std::lock_guard<std::mutex> lock(mtx_);

    uint32_t available = write_pos_ - read_pos_;
    if (available == 0) return 0;

    uint32_t to_pop = std::min(static_cast<uint32_t>(max_count), available);
    for (uint32_t i = 0; i < to_pop; ++i) {
        out.push_back(std::move(buffer_[(read_pos_ + i) & mask_]));
    }

    read_pos_ += to_pop;
    return to_pop;
}

uint32_t DemotionQueue::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return unlocked_size();
}

bool DemotionQueue::is_under_pressure() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return unlocked_size() >= static_cast<uint32_t>(capacity_ * pressure_threshold_);
}

uint32_t DemotionQueue::next_power_of_2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

}  // namespace walde
