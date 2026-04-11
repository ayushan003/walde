#include "striped_cache.h"

#include <array>
#include <xxhash.h>

namespace walde {

StripedCache::StripedCache(const Config& config)
    : slab_(config.total_capacity + kNumStripes)
{
    uint32_t per_stripe = config.total_capacity / kNumStripes;
    if (per_stripe < 3) per_stripe = 3;

    for (uint32_t i = 0; i < kNumStripes; ++i) {
        stripes_[i] = std::make_unique<CacheStripe>(
            slab_, per_stripe, config.window_pct, config.probation_pct);
    }
}

std::optional<std::string> StripedCache::get(const std::string& key,
                                              LatencyBreakdown* bd) {
    return stripes_[route(key)]->get(key, bd);
}

bool StripedCache::put(const std::string& key, const std::string& value,
                       LatencyBreakdown* bd) {
    return stripes_[route(key)]->put(key, value, bd);
}

bool StripedCache::remove(const std::string& key) {
    return stripes_[route(key)]->remove(key);
}

std::vector<std::optional<std::string>> StripedCache::batch_get(
        const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> results(keys.size());

    // Group key indices by stripe to acquire each lock at most once
    std::array<std::vector<size_t>, kNumStripes> stripe_groups;
    for (size_t i = 0; i < keys.size(); ++i) {
        stripe_groups[route(keys[i])].push_back(i);
    }

    for (uint32_t s = 0; s < kNumStripes; ++s) {
        if (stripe_groups[s].empty()) continue;
        // All keys in this group share the same stripe lock
        for (size_t idx : stripe_groups[s]) {
            results[idx] = stripes_[s]->get(keys[idx]);
        }
    }

    return results;
}

uint64_t StripedCache::total_hits() const {
    uint64_t sum = 0;
    for (auto& s : stripes_) sum += s->hit_count();
    return sum;
}

uint64_t StripedCache::total_misses() const {
    uint64_t sum = 0;
    for (auto& s : stripes_) sum += s->miss_count();
    return sum;
}

uint64_t StripedCache::total_evictions() const {
    uint64_t sum = 0;
    for (auto& s : stripes_) sum += s->eviction_count();
    return sum;
}

uint64_t StripedCache::total_admissions() const {
    uint64_t sum = 0;
    for (auto& s : stripes_) sum += s->admission_count();
    return sum;
}

uint64_t StripedCache::total_rejections() const {
    uint64_t sum = 0;
    for (auto& s : stripes_) sum += s->rejection_count();
    return sum;
}

uint32_t StripedCache::total_size() const {
    uint32_t sum = 0;
    for (auto& s : stripes_) sum += s->size();
    return sum;
}

uint64_t StripedCache::stripe_hits(uint32_t stripe_id) const {
    return stripes_[stripe_id]->hit_count();
}

uint64_t StripedCache::stripe_misses(uint32_t stripe_id) const {
    return stripes_[stripe_id]->miss_count();
}

uint32_t StripedCache::route(const std::string& key) const {
    uint64_t h = XXH64(key.data(), key.size(), 0);
    return static_cast<uint32_t>(h % kNumStripes);
}

}  // namespace walde
