#include "demotion_queue.h"

#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using namespace walde;

TEST(DemotionQueue, PushPopRoundTrip) {
    DemotionQueue q(16);

    DemotionItem item{"key1", "val1", 5};
    EXPECT_TRUE(q.try_push(std::move(item)));
    EXPECT_EQ(q.size(), 1u);

    DemotionItem out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.key, "key1");
    EXPECT_EQ(out.value, "val1");
    EXPECT_EQ(out.frequency, 5u);
    EXPECT_EQ(q.size(), 0u);
}

TEST(DemotionQueue, PopFromEmptyReturnsFalse) {
    DemotionQueue q(16);
    DemotionItem out;
    EXPECT_FALSE(q.try_pop(out));
}

TEST(DemotionQueue, FIFOOrder) {
    DemotionQueue q(16);

    for (int i = 0; i < 8; ++i) {
        q.try_push({"k" + std::to_string(i), "v", 0});
    }

    for (int i = 0; i < 8; ++i) {
        DemotionItem out;
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.key, "k" + std::to_string(i));
    }
}

TEST(DemotionQueue, BackpressureDropsItems) {
    DemotionQueue q(16);  // Rounded to 16

    // Fill to ~80% (threshold)
    int pushed = 0;
    for (int i = 0; i < 20; ++i) {
        if (q.try_push({"k" + std::to_string(i), "v", 0})) {
            ++pushed;
        }
    }

    // Some should have been dropped due to backpressure or full
    EXPECT_GT(q.drops(), 0u);
    EXPECT_LE(q.size(), 16u);
}

TEST(DemotionQueue, DrainAfterBackpressure) {
    DemotionQueue q(16);

    // Fill up
    for (int i = 0; i < 12; ++i) {
        q.try_push({"k" + std::to_string(i), "v", 0});
    }

    // Drain some
    for (int i = 0; i < 8; ++i) {
        DemotionItem out;
        q.try_pop(out);
    }

    // Should be able to push again
    EXPECT_TRUE(q.try_push({"new_key", "new_val", 0}));
}

TEST(DemotionQueue, CapacityRoundedToPowerOf2) {
    DemotionQueue q1(10);
    EXPECT_EQ(q1.capacity(), 16u);

    DemotionQueue q2(16);
    EXPECT_EQ(q2.capacity(), 16u);

    DemotionQueue q3(17);
    EXPECT_EQ(q3.capacity(), 32u);
}

TEST(DemotionQueue, ConcurrentProducersSingleConsumer) {
    DemotionQueue q(4096);
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 500;

    std::atomic<int> total_pushed{0};
    std::atomic<int> total_popped{0};
    std::atomic<bool> done{false};

    // Producer threads
    auto producer = [&](int tid) {
        for (int i = 0; i < kItemsPerProducer; ++i) {
            DemotionItem item{
                "t" + std::to_string(tid) + "_k" + std::to_string(i),
                "v", static_cast<uint32_t>(i)
            };
            if (q.try_push(std::move(item))) {
                total_pushed.fetch_add(1);
            }
        }
    };

    // Single consumer thread
    auto consumer = [&]() {
        while (!done.load() || q.size() > 0) {
            DemotionItem out;
            if (q.try_pop(out)) {
                total_popped.fetch_add(1);
                EXPECT_FALSE(out.key.empty());
            }
        }
    };

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back(producer, t);
    }

    std::thread consumer_thread(consumer);

    for (auto& t : producers) {
        t.join();
    }
    done.store(true);
    consumer_thread.join();

    // pushed = popped + remaining + drops
    EXPECT_EQ(total_pushed.load(), total_popped.load());
    EXPECT_EQ(q.size(), 0u);
}
