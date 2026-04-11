#pragma once

#include "demotion_queue.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace walde {

class DemotionDrainer {
public:
    using InsertFn = std::function<void(const std::vector<DemotionItem>&)>;

    DemotionDrainer(DemotionQueue& queue, InsertFn insert_fn);
    ~DemotionDrainer();

    void stop();
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
