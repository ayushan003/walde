#include "demotion_drainer.h"
#include "demotion_queue.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace walde;

TEST(DemotionDrainer, DrainsItemsToCallback) {
    DemotionQueue queue(256);
    std::mutex map_mutex;
    std::unordered_map<std::string, std::string> l2;

    {
        DemotionDrainer drainer(queue, [&](const std::vector<DemotionItem>& batch) {
            std::lock_guard<std::mutex> lock(map_mutex);
            for (const auto& item : batch) {
                l2[item.key] = item.value;
            }
        });

        // Push items
        for (int i = 0; i < 50; ++i) {
            queue.try_push({"k" + std::to_string(i), "v" + std::to_string(i), 0});
            drainer.notify();
        }

        // Give the drainer time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Drainer destructor calls stop() which does final drain

    // All items should have been drained into l2
    std::lock_guard<std::mutex> lock(map_mutex);
    EXPECT_EQ(l2.size(), 50u);
    EXPECT_EQ(l2["k0"], "v0");
    EXPECT_EQ(l2["k49"], "v49");
}

TEST(DemotionDrainer, StopsCleanlyOnEmptyQueue) {
    DemotionQueue queue(256);
    std::atomic<int> count{0};

    {
        DemotionDrainer drainer(queue, [&](const std::vector<DemotionItem>& batch) {
            count.fetch_add(batch.size());
        });

        // Don't push anything — drainer should idle
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        drainer.stop();

        EXPECT_EQ(count.load(), 0);
        EXPECT_FALSE(drainer.is_running());
    }
}

TEST(DemotionDrainer, FinalDrainOnDestruction) {
    DemotionQueue queue(256);
    std::atomic<int> count{0};

    // Push items BEFORE creating drainer
    for (int i = 0; i < 20; ++i) {
        queue.try_push({"k" + std::to_string(i), "v", 0});
    }

    {
        DemotionDrainer drainer(queue, [&](const std::vector<DemotionItem>& batch) {
            count.fetch_add(batch.size());
        });
        // Destructor fires immediately — should do final drain
    }

    EXPECT_EQ(count.load(), 20);
}

TEST(DemotionDrainer, ConcurrentPushAndDrain) {
    DemotionQueue queue(4096);
    std::atomic<int> drained_count{0};
    constexpr int kItems = 2000;

    {
        DemotionDrainer drainer(queue, [&](const std::vector<DemotionItem>& batch) {
            drained_count.fetch_add(batch.size());
        });

        // Producer thread pushing while drainer is running
        std::thread producer([&]() {
            for (int i = 0; i < kItems; ++i) {
                while (!queue.try_push({"k" + std::to_string(i), "v", 0})) {
                    std::this_thread::yield();
                }
                if (i % 10 == 0) drainer.notify();
            }
        });

        producer.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(drained_count.load(), kItems);
}
