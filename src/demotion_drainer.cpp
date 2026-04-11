#include "demotion_drainer.h"

#include <chrono>

namespace walde {

DemotionDrainer::DemotionDrainer(DemotionQueue& queue, InsertFn insert_fn)
    : queue_(queue)
    , insert_fn_(std::move(insert_fn))
    , thread_(&DemotionDrainer::run, this)
{}

DemotionDrainer::~DemotionDrainer() {
    stop();
}

void DemotionDrainer::stop() {
    if (!running_.exchange(false)) return;
    wake_cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DemotionDrainer::notify() {
    wake_cv_.notify_one();
}

void DemotionDrainer::run() {
    std::vector<DemotionItem> batch;
    batch.reserve(64);

    while (running_.load(std::memory_order_relaxed)) {
        batch.clear();
        size_t popped = queue_.try_pop_batch(batch, 64);
        if (popped > 0) {
            insert_fn_(batch);
            drained_.fetch_add(popped, std::memory_order_relaxed);
        } else {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(1),
                [this] { return !running_.load(std::memory_order_relaxed)
                                || queue_.size() > 0; });
        }
    }

    // Final drain
    batch.clear();
    while (queue_.try_pop_batch(batch, 64) > 0) {
        insert_fn_(batch);
        drained_.fetch_add(batch.size(), std::memory_order_relaxed);
        batch.clear();
    }
}

}  // namespace walde
