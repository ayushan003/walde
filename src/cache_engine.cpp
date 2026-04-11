#include "cache_engine.h"

#include <chrono>
#include <xxhash.h>

namespace walde {

using Clock = std::chrono::steady_clock;

CacheEngine::CacheEngine(const Config& config,
                         std::unique_ptr<StorageBackend> backend)
    : l1_(config.l1_config)
    , backend_(std::move(backend))
    , demotion_queue_(config.demotion_queue_size)
{
    l2_capacity_per_stripe_ = std::max(1u,
        config.l2_capacity / static_cast<uint32_t>(kNumL2Stripes));

    for (size_t i = 0; i < kNumL2Stripes; ++i) {
        l2_stripes_[i] = std::make_unique<L2Stripe>();
        l2_stripes_[i]->map.reserve(l2_capacity_per_stripe_);
    }

    l1_.set_demotion_queue(&demotion_queue_);

    drainer_ = std::make_unique<DemotionDrainer>(
        demotion_queue_,
        [this](const std::vector<DemotionItem>& batch) {
            l2_put_batch(batch);
        });
}

CacheEngine::~CacheEngine() {
    l1_.set_demotion_queue(nullptr);
    if (drainer_) {
        drainer_->stop();
        drainer_.reset();
    }
}

// ─── GET ────────────────────────────────────────────────────
//
// L1 hit path: zero L2 interaction, zero backend interaction.
// Only on L1 miss do we check L2, then backend.

std::optional<std::string> CacheEngine::get(const std::string& key,
                                             LatencyBreakdown* bd) {
    // ── L1 lookup ───────────────────────────────────────────
    auto l1_result = l1_.get(key, bd);
    if (l1_result.has_value()) {
        if (bd) bd->path = LatencyBreakdown::Path::L1_HIT;
        return l1_result;
    }

    // ── L2 lookup (only on L1 miss) ─────────────────────────
    int64_t l2_ns = 0;
    auto l2_result = l2_get(key, bd ? &l2_ns : nullptr);
    if (bd) bd->l2_ns = l2_ns;

    if (l2_result.has_value()) {
        if (bd) bd->path = LatencyBreakdown::Path::L2_HIT;
        // Re-insert into L1 with instrumentation so slab/eviction
        // costs of the rescue path are captured in the breakdown.
        l1_.put(key, *l2_result, bd);
        return l2_result;
    }

    // ── Backend lookup ──────────────────────────────────────
    if (bd) {
        auto backend_start = Clock::now();
        auto backend_result = backend_->get(key);
        auto backend_end = Clock::now();
        bd->backend_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(backend_end - backend_start).count();

        if (backend_result.has_value()) {
            bd->path = LatencyBreakdown::Path::BACKEND_HIT;
            l1_.put(key, *backend_result, bd);
            return backend_result;
        }
        bd->path = LatencyBreakdown::Path::MISS;
        return std::nullopt;
    }

    // Non-instrumented backend path
    auto backend_result = backend_->get(key);
    if (backend_result.has_value()) {
        l1_.put(key, *backend_result);
        return backend_result;
    }
    return std::nullopt;
}

bool CacheEngine::put(const std::string& key, const std::string& value,
                      LatencyBreakdown* bd) {
    backend_->put(key, value);
    return l1_.put(key, value, bd);
}

bool CacheEngine::remove(const std::string& key) {
    bool in_l1 = l1_.remove(key);

    {
        size_t idx = get_l2_stripe_index(key);
        auto& stripe = *l2_stripes_[idx];
        std::lock_guard<std::mutex> lock(stripe.mutex);
        auto it = stripe.map.find(key);
        if (it != stripe.map.end()) {
            stripe.list.erase(it->second);
            stripe.map.erase(it);
        }
    }

    bool in_backend = backend_->remove(key);
    return in_l1 || in_backend;
}

// ─── L2 ─────────────────────────────────────────────────────

size_t CacheEngine::get_l2_stripe_index(const std::string& key) const {
    // Use XXH64 for consistent hashing with L1 routing.
    uint64_t h = XXH64(key.data(), key.size(), 0x1234567890ABCDEFULL);
    return static_cast<size_t>(h % kNumL2Stripes);
}

std::optional<std::string> CacheEngine::l2_get(const std::string& key,
                                                int64_t* l2_ns) {
    auto start = (l2_ns) ? Clock::now() : Clock::time_point{};

    size_t idx = get_l2_stripe_index(key);
    auto& stripe = *l2_stripes_[idx];

    std::lock_guard<std::mutex> lock(stripe.mutex);

    auto it = stripe.map.find(key);
    if (it == stripe.map.end()) {
        stripe.misses.fetch_add(1, std::memory_order_relaxed);
        if (l2_ns) {
            *l2_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start).count();
        }
        return std::nullopt;
    }

    // Move value out (exclusive: remove from L2 on hit)
    std::string value = std::move(it->second->value);
    stripe.list.erase(it->second);
    stripe.map.erase(it);

    stripe.hits.fetch_add(1, std::memory_order_relaxed);
    if (l2_ns) {
        *l2_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count();
    }
    return value;
}

void CacheEngine::l2_put_batch(const std::vector<DemotionItem>& batch) {
    if (batch.empty()) return;

    for (const auto& item : batch) {
        size_t idx = get_l2_stripe_index(item.key);
        auto& stripe = *l2_stripes_[idx];

        std::lock_guard<std::mutex> lock(stripe.mutex);

        auto it = stripe.map.find(item.key);
        if (it != stripe.map.end()) {
            it->second->value = item.value;
            stripe.list.splice(stripe.list.begin(), stripe.list, it->second);
            continue;
        }

        if (stripe.map.size() >= l2_capacity_per_stripe_) {
            const std::string& lru_key = stripe.list.back().key;
            stripe.map.erase(lru_key);
            stripe.list.pop_back();
        }

        stripe.list.push_front({item.key, item.value});
        stripe.map[item.key] = stripe.list.begin();
    }
}

// ─── Stats ──────────────────────────────────────────────────

uint64_t CacheEngine::l2_hits() const {
    uint64_t total = 0;
    for (const auto& s : l2_stripes_)
        total += s->hits.load(std::memory_order_relaxed);
    return total;
}

uint64_t CacheEngine::l2_misses() const {
    uint64_t total = 0;
    for (const auto& s : l2_stripes_)
        total += s->misses.load(std::memory_order_relaxed);
    return total;
}

uint32_t CacheEngine::l2_size() const {
    uint32_t total = 0;
    for (const auto& s : l2_stripes_) {
        std::lock_guard<std::mutex> lock(s->mutex);
        total += static_cast<uint32_t>(s->map.size());
    }
    return total;
}

double CacheEngine::l1_hit_rate() const {
    uint64_t total = l1_.total_hits() + l1_.total_misses();
    if (total == 0) return 0.0;
    return static_cast<double>(l1_.total_hits()) / total;
}

double CacheEngine::overall_hit_rate() const {
    uint64_t total = l1_.total_hits() + l1_.total_misses();
    if (total == 0) return 0.0;
    uint64_t hits = l1_.total_hits() + l2_hits();
    return static_cast<double>(hits) / total;
}

}  // namespace walde
