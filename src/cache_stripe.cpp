#include "cache_stripe.h"

#include <algorithm>
#include <cassert>
#include <chrono>

namespace walde {

CacheStripe::CacheStripe(SlabAllocator& slab, uint32_t capacity,
                         float window_pct, float probation_pct)
    : slab_(slab)
    , total_capacity_(capacity)
    , window_(slab)
    , probation_(slab)
    , protected_(slab)
    , cms_(4, 8192)
{
    assert(capacity >= 3);

    window_capacity_ = std::max(3u, static_cast<uint32_t>(capacity * window_pct));
    if (window_capacity_ >= capacity - 2) {
        window_capacity_ = capacity / 4;
    }

    main_capacity_      = capacity - window_capacity_;
    probation_capacity_ = std::max(1u, static_cast<uint32_t>(main_capacity_ * probation_pct));
    protected_capacity_ = main_capacity_ - probation_capacity_;

    decay_interval_ = static_cast<uint64_t>(capacity) * 5;
    if (decay_interval_ == 0) decay_interval_ = 1;

    index_map_.reserve(capacity);
}

std::optional<std::string> CacheStripe::get(const std::string& key,
                                             LatencyBreakdown* bd) {
    auto lock_start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto lock_end = std::chrono::steady_clock::now();
    if (bd) {
        bd->lock_wait_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(lock_end - lock_start).count();
    }

    auto lookup_start = std::chrono::steady_clock::now();

    auto it = index_map_.find(key);
    if (it == index_map_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        if (bd) {
            bd->lookup_ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - lookup_start).count();
        }
        return std::nullopt;
    }

    cms_.increment(key);
    maybe_decay_cms();

    uint32_t idx    = it->second;
    CacheNode& node = slab_.node(idx);

    auto seg = static_cast<Segment>(node.segment);
    switch (seg) {
        case Segment::Window:    window_.move_to_front(idx);    break;
        case Segment::Probation: promote_to_protected(idx);     break;
        case Segment::Protected: protected_.move_to_front(idx); break;
        default:
            assert(false && "Invalid segment");
            break;
    }

    if (bd) {
        bd->lookup_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - lookup_start).count();
    }

    hits_.fetch_add(1, std::memory_order_relaxed);
    return node.value;
}

bool CacheStripe::put(const std::string& key, const std::string& value,
                      LatencyBreakdown* bd) {
    auto lock_start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto lock_end = std::chrono::steady_clock::now();
    if (bd) {
        bd->lock_wait_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(lock_end - lock_start).count();
    }

    cms_.increment(key);
    maybe_decay_cms();

    auto lookup_start = std::chrono::steady_clock::now();
    auto it = index_map_.find(key);

    if (it != index_map_.end()) {
        uint32_t idx    = it->second;
        CacheNode& node = slab_.node(idx);
        node.value = value;

        auto seg = static_cast<Segment>(node.segment);
        switch (seg) {
            case Segment::Window:    window_.move_to_front(idx);    break;
            case Segment::Probation: promote_to_protected(idx);     break;
            case Segment::Protected: protected_.move_to_front(idx); break;
            default: break;
        }

        if (bd) {
            bd->lookup_ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - lookup_start).count();
        }
        return false;  // Key already existed; updated in place
    }

    if (bd) {
        bd->lookup_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - lookup_start).count();
    }

    // Allocate a slab slot for the new item
    auto slab_start = std::chrono::steady_clock::now();
    uint32_t idx = slab_.allocate();
    if (bd) {
        bd->slab_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - slab_start).count();
    }

    if (idx == kInvalidIndex) {
        return false;  // Pool exhausted
    }

    CacheNode& node = slab_.node(idx);
    node.key     = key;
    node.value   = value;
    node.segment = static_cast<uint8_t>(Segment::Window);

    index_map_[key] = idx;
    insert_into_window(idx, bd);

    return true;
}

bool CacheStripe::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = index_map_.find(key);
    if (it == index_map_.end()) return false;

    uint32_t idx = it->second;
    auto seg = static_cast<Segment>(slab_.node(idx).segment);

    switch (seg) {
        case Segment::Window:    window_.remove(idx);    break;
        case Segment::Probation: probation_.remove(idx); break;
        case Segment::Protected: protected_.remove(idx); break;
        default: break;
    }

    index_map_.erase(it);
    slab_.deallocate(idx);
    return true;
}

// ─── Internal ───────────────────────────────────────────────

void CacheStripe::insert_into_window(uint32_t idx, LatencyBreakdown* bd) {
    window_.push_front(idx);

    while (window_.size() > window_capacity_) {
        try_admit_from_window(bd);
    }
}

void CacheStripe::try_admit_from_window(LatencyBreakdown* bd) {
    uint32_t candidate_idx = window_.pop_back();
    if (candidate_idx == kInvalidIndex) return;

    CacheNode& candidate = slab_.node(candidate_idx);
    uint32_t main_size   = probation_.size() + protected_.size();

    if (main_size < main_capacity_) {
        // Main has room — admit directly to probation
        candidate.segment = static_cast<uint8_t>(Segment::Probation);
        probation_.push_front(candidate_idx);
        admissions_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (probation_.empty()) {
        // No victim to compare against — reject candidate
        index_map_.erase(candidate.key);
        slab_.deallocate(candidate_idx);
        rejections_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Admission gate: candidate must strictly beat probation LRU victim
    auto admit_start = std::chrono::steady_clock::now();

    uint32_t victim_idx    = probation_.peek_back();
    CacheNode& victim      = slab_.node(victim_idx);
    uint32_t candidate_freq = cms_.estimate(candidate.key);
    uint32_t victim_freq    = cms_.estimate(victim.key);

    if (bd) {
        bd->admission_ns += std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - admit_start).count();
    }

    if (candidate_freq > victim_freq) {
        auto evict_start = std::chrono::steady_clock::now();

        probation_.pop_back();
        demote_victim(victim_idx);

        candidate.segment = static_cast<uint8_t>(Segment::Probation);
        probation_.push_front(candidate_idx);

        if (bd) {
            bd->eviction_ns += std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - evict_start).count();
        }

        admissions_.fetch_add(1, std::memory_order_relaxed);
    } else {
        index_map_.erase(candidate.key);
        slab_.deallocate(candidate_idx);
        rejections_.fetch_add(1, std::memory_order_relaxed);
    }
}

void CacheStripe::promote_to_protected(uint32_t idx) {
    CacheNode& node = slab_.node(idx);
    probation_.remove(idx);

    if (protected_.size() >= protected_capacity_) {
        // Protected is full — demote its LRU to probation
        uint32_t demoted_idx = protected_.pop_back();
        if (demoted_idx != kInvalidIndex) {
            slab_.node(demoted_idx).segment =
                static_cast<uint8_t>(Segment::Probation);
            probation_.push_front(demoted_idx);
        }
    }

    node.segment = static_cast<uint8_t>(Segment::Protected);
    protected_.push_front(idx);
}

void CacheStripe::demote_victim(uint32_t victim_idx) {
    CacheNode& victim = slab_.node(victim_idx);
    index_map_.erase(victim.key);

    if (demotion_queue_) {
        DemotionItem di{victim.key, victim.value,
                        cms_.estimate(victim.key)};
        slab_.deallocate(victim_idx);
        demotion_queue_->try_push(std::move(di));
    } else {
        slab_.deallocate(victim_idx);
    }

    evictions_.fetch_add(1, std::memory_order_relaxed);
}

void CacheStripe::maybe_decay_cms() {
    // FIX: Use a separate counter incremented after each CMS op.
    // This avoids the modulo-0 bug where decay triggered before the
    // first real increment (0 % interval == 0).
    ++ops_since_decay_;
    if (ops_since_decay_ >= decay_interval_) {
        cms_.decay();
        ops_since_decay_ = 0;
    }
}

uint32_t CacheStripe::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return window_.size() + probation_.size() + protected_.size();
}

}  // namespace walde
