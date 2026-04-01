#pragma once

#include "demotion_queue.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace walde {

// ─── DemotionDrainer ────────────────────────────────────────
//
// Background thread that drains items from DemotionQueue into L2.
// Runs a tight drain-or-sleep loop:
//   - If items are available: drain a batch of up to 64, call insert_fn.
//   - If queue is empty: sleep up to 1ms (or until notified).
//
// Lifecycle: constructor starts the thread. stop() drains remaining
// items, joins the thread. Destructor calls stop().

class DemotionDrainer {
public:
    using InsertFn = std::function<void(const std::vector<DemotionItem>&)>;

    DemotionDrainer(DemotionQueue& queue, InsertFn insert_fn);
    ~DemotionDrainer();

    // Signal and join the background thread. Safe to call multiple times.
    void stop();

    // Wake the drainer thread immediately (call after push to L1 eviction).
    void notify();

    uint64_t total_drained() const {
        return drained_.load(std::memory_order_relaxed);
    }

    bool is_running() const {
        return running_.load(std::memory_order_relaxed);
    }

private:
    void run();

    DemotionQueue&          queue_;
    InsertFn                insert_fn_;
    std::atomic<bool>       running_{true};
    std::atomic<uint64_t>   drained_{0};
    std::mutex              wake_mutex_;
    std::condition_variable wake_cv_;
    std::thread             thread_;
};

}  // namespace walde
