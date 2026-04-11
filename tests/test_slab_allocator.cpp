#include "slab_allocator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <thread>
#include <vector>

using namespace walde;

// ─── Basic allocation ───────────────────────────────────────

TEST(SlabAllocator, ConstructionSetsCapacity) {
    SlabAllocator slab(256);
    EXPECT_EQ(slab.capacity(), 256u);
    EXPECT_EQ(slab.available(), 256u);
}

TEST(SlabAllocator, ZeroCapacityThrows) {
    EXPECT_THROW(SlabAllocator(0), std::invalid_argument);
}

TEST(SlabAllocator, SingleAllocDeallocRoundTrip) {
    SlabAllocator slab(16);

    uint32_t idx = slab.allocate();
    ASSERT_NE(idx, kInvalidIndex);
    EXPECT_EQ(slab.available(), 15u);

    // Write to the node
    auto& node = slab.node(idx);
    node.key   = "test_key";
    node.value = "test_value";
    EXPECT_EQ(node.key, "test_key");

    // Deallocate resets the node
    slab.deallocate(idx);
    EXPECT_EQ(slab.available(), 16u);

    // Node should be fully reset after dealloc.
    // With separate free-list linkage, CacheNode::next is NOT used
    // for free-list threading — it stays at kInvalidIndex after reset().
    EXPECT_EQ(slab.node(idx).key, "");
    EXPECT_EQ(slab.node(idx).value, "");
    EXPECT_EQ(slab.node(idx).prev, kInvalidIndex);
    EXPECT_EQ(slab.node(idx).next, kInvalidIndex);
    EXPECT_EQ(slab.node(idx).segment, 0);
}

// ─── Exhaustion & uniqueness ────────────────────────────────

TEST(SlabAllocator, AllocateAllReturnsUniqueIndices) {
    constexpr uint32_t N = 128;
    SlabAllocator slab(N);

    std::set<uint32_t> indices;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t idx = slab.allocate();
        ASSERT_NE(idx, kInvalidIndex) << "Failed at allocation " << i;
        auto [_, inserted] = indices.insert(idx);
        EXPECT_TRUE(inserted) << "Duplicate index " << idx << " at allocation " << i;
    }

    EXPECT_EQ(indices.size(), N);
    EXPECT_EQ(slab.available(), 0u);
}

TEST(SlabAllocator, ExhaustedPoolReturnsInvalid) {
    SlabAllocator slab(4);

    for (int i = 0; i < 4; ++i) {
        ASSERT_NE(slab.allocate(), kInvalidIndex);
    }

    EXPECT_EQ(slab.allocate(), kInvalidIndex);
    EXPECT_EQ(slab.available(), 0u);
}

// ─── Reuse after deallocation ───────────────────────────────

TEST(SlabAllocator, DeallocatedNodesAreReusable) {
    SlabAllocator slab(4);

    std::vector<uint32_t> all;
    for (int i = 0; i < 4; ++i) {
        all.push_back(slab.allocate());
    }
    EXPECT_EQ(slab.allocate(), kInvalidIndex);

    slab.deallocate(all[1]);
    slab.deallocate(all[3]);
    EXPECT_EQ(slab.available(), 2u);

    uint32_t a = slab.allocate();
    uint32_t b = slab.allocate();
    ASSERT_NE(a, kInvalidIndex);
    ASSERT_NE(b, kInvalidIndex);
    EXPECT_NE(a, b);

    std::set<uint32_t> freed = {all[1], all[3]};
    EXPECT_TRUE(freed.count(a) && freed.count(b));

    EXPECT_EQ(slab.allocate(), kInvalidIndex);
}

TEST(SlabAllocator, AllocDeallocCycleStressSequential) {
    SlabAllocator slab(64);

    for (int cycle = 0; cycle < 100; ++cycle) {
        std::vector<uint32_t> batch;
        for (int i = 0; i < 32; ++i) {
            uint32_t idx = slab.allocate();
            ASSERT_NE(idx, kInvalidIndex);
            slab.node(idx).key = "k" + std::to_string(cycle * 32 + i);
            batch.push_back(idx);
        }
        for (uint32_t idx : batch) {
            slab.deallocate(idx);
        }
        EXPECT_EQ(slab.available(), 64u);
    }
}

// ─── Node access correctness ────────────────────────────────

TEST(SlabAllocator, NodeDataPersistsBetweenAccesses) {
    SlabAllocator slab(8);

    uint32_t a = slab.allocate();
    uint32_t b = slab.allocate();

    slab.node(a).key   = "alpha";
    slab.node(a).value = "one";
    slab.node(b).key   = "beta";
    slab.node(b).value = "two";

    EXPECT_EQ(slab.node(a).key, "alpha");
    EXPECT_EQ(slab.node(b).key, "beta");
    EXPECT_EQ(slab.node(a).value, "one");
    EXPECT_EQ(slab.node(b).value, "two");
}

TEST(SlabAllocator, NodeSegmentAndFrequencyWork) {
    SlabAllocator slab(4);
    uint32_t idx = slab.allocate();

    auto& node = slab.node(idx);
    node.segment = static_cast<uint8_t>(Segment::Window);

    EXPECT_EQ(node.segment, 1);

    slab.deallocate(idx);
    EXPECT_EQ(slab.node(idx).segment, 0);
}

// ─── Concurrent stress test ─────────────────────────────────

TEST(SlabAllocator, ConcurrentAllocDeallocNoCorruption) {
    constexpr uint32_t kCapacity    = 1024;
    constexpr int      kThreads     = 8;
    constexpr int      kOpsPerThread = 5000;

    SlabAllocator slab(kCapacity);

    auto worker = [&]() {
        std::vector<uint32_t> held;
        held.reserve(32);

        for (int i = 0; i < kOpsPerThread; ++i) {
            if (held.size() < 16) {
                uint32_t idx = slab.allocate();
                if (idx != kInvalidIndex) {
                    slab.node(idx).key = "t" + std::to_string(i);
                    held.push_back(idx);
                }
            } else {
                for (size_t j = 0; j < held.size() / 2; ++j) {
                    slab.deallocate(held.back());
                    held.pop_back();
                }
            }
        }

        for (uint32_t idx : held) {
            slab.deallocate(idx);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(slab.available(), kCapacity);

    // Verify the free list is intact — allocate all, check uniqueness
    std::set<uint32_t> post_indices;
    for (uint32_t i = 0; i < kCapacity; ++i) {
        uint32_t idx = slab.allocate();
        ASSERT_NE(idx, kInvalidIndex) << "Free list corrupted at allocation " << i;
        auto [_, ok] = post_indices.insert(idx);
        EXPECT_TRUE(ok) << "Duplicate index " << idx << " after concurrent stress";
    }
    EXPECT_EQ(post_indices.size(), kCapacity);
    EXPECT_EQ(slab.allocate(), kInvalidIndex);
}

TEST(SlabAllocator, ConcurrentAllUniqueUnderContention) {
    constexpr uint32_t kCapacity = 256;
    constexpr int      kThreads  = 8;

    SlabAllocator slab(kCapacity);

    std::vector<std::vector<uint32_t>> per_thread(kThreads);

    auto grabber = [&](int tid) {
        while (true) {
            uint32_t idx = slab.allocate();
            if (idx == kInvalidIndex) break;
            per_thread[tid].push_back(idx);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(grabber, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    std::set<uint32_t> all;
    size_t total = 0;
    for (auto& vec : per_thread) {
        for (uint32_t idx : vec) {
            all.insert(idx);
            ++total;
        }
    }

    EXPECT_EQ(total, kCapacity);
    EXPECT_EQ(all.size(), kCapacity);
}
