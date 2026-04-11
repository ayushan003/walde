#include "cache_stripe.h"
#include "slab_allocator.h"

#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using namespace walde;

class CacheStripeTest : public ::testing::Test {
protected:
    static constexpr uint32_t kSlabSize = 4096;
    SlabAllocator slab{kSlabSize};
};

// ─── Basic get/put ──────────────────────────────────────────

TEST_F(CacheStripeTest, PutAndGetRoundTrip) {
    CacheStripe stripe(slab, 16);

    EXPECT_TRUE(stripe.put("hello", "world"));
    auto val = stripe.get("hello");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "world");
}

TEST_F(CacheStripeTest, GetMissReturnsNullopt) {
    CacheStripe stripe(slab, 16);
    EXPECT_FALSE(stripe.get("nonexistent").has_value());
    EXPECT_EQ(stripe.miss_count(), 1u);
}

TEST_F(CacheStripeTest, PutUpdateOverwritesValue) {
    CacheStripe stripe(slab, 16);

    stripe.put("key", "v1");
    EXPECT_FALSE(stripe.put("key", "v2"));

    auto val = stripe.get("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v2");
}

// ─── Remove ─────────────────────────────────────────────────

TEST_F(CacheStripeTest, RemoveExistingKey) {
    CacheStripe stripe(slab, 16);

    stripe.put("x", "y");
    EXPECT_TRUE(stripe.remove("x"));
    EXPECT_FALSE(stripe.get("x").has_value());
}

TEST_F(CacheStripeTest, RemoveNonexistentReturnsFalse) {
    CacheStripe stripe(slab, 16);
    EXPECT_FALSE(stripe.remove("ghost"));
}

// ─── Eviction under capacity ────────────────────────────────

TEST_F(CacheStripeTest, DoesNotExceedCapacity) {
    CacheStripe stripe(slab, 20);

    for (int i = 0; i < 100; ++i) {
        stripe.put("k" + std::to_string(i), "v" + std::to_string(i));
    }

    EXPECT_LE(stripe.size(), 20u);
}

TEST_F(CacheStripeTest, RecentItemsSurvive) {
    // With W-TinyLFU, recently inserted items go to the window.
    // The most recent ones should still be accessible.
    CacheStripe stripe(slab, 20);

    for (int i = 0; i < 50; ++i) {
        stripe.put("k" + std::to_string(i), "v");
    }

    // The very last item inserted should be in the window
    auto val = stripe.get("k49");
    EXPECT_TRUE(val.has_value());
}

// ─── SCAN RESISTANCE (the core W-TinyLFU test) ─────────────

TEST_F(CacheStripeTest, ScanResistance) {
    // This is the most important test in the project.
    //
    // Setup: cache of 200 items. Build a hot working set of 80 keys,
    // each accessed many times to build strong CMS frequency.
    //
    // Attack: flood with 1000 unique sequential "scan" keys (each
    // accessed only once — simulating a database table scan).
    //
    // Expectation: hot keys should mostly survive because W-TinyLFU
    // rejects low-frequency scan keys at the admission gate.
    // A standard LRU would lose ALL hot keys.

    CacheStripe stripe(slab, 200, 0.05f, 0.20f);

    // Phase 1: Build hot working set with heavy repeated access.
    // Each key gets put 5 times + get 10 times = 15 CMS increments.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 80; ++i) {
            stripe.put("hot_" + std::to_string(i), "value");
        }
    }
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 80; ++i) {
            stripe.get("hot_" + std::to_string(i));
        }
    }

    // Phase 2: Scan attack — 1000 unique one-shot keys
    for (int i = 0; i < 1000; ++i) {
        stripe.put("scan_" + std::to_string(i), "junk");
    }

    // Phase 3: Check how many hot keys survived.
    // NOTE: we use get() here which itself touches CMS, but
    // the hot keys already have counts of ~15 so this is fine.
    int hot_survived = 0;
    for (int i = 0; i < 80; ++i) {
        if (stripe.get("hot_" + std::to_string(i)).has_value()) {
            ++hot_survived;
        }
    }

    // W-TinyLFU should protect the majority of the hot set.
    // We expect at least 50% survival (40/80). Pure LRU would
    // give 0% because 1000 scan keys would flush everything.
    EXPECT_GE(hot_survived, 40)
        << "Only " << hot_survived << "/80 hot keys survived scan attack. "
        << "W-TinyLFU admission should protect the hot working set.";

    // Also verify that admissions + rejections happened —
    // proving the gate is actually active, not just letting everything through.
    EXPECT_GT(stripe.rejection_count(), 0u)
        << "No rejections recorded — W-TinyLFU gate may not be firing.";
    EXPECT_GT(stripe.admission_count(), 0u)
        << "No admissions recorded — nothing got through the gate.";
}

// ─── SLRU promotion ─────────────────────────────────────────

TEST_F(CacheStripeTest, RepeatedAccessPromotesToProtected) {
    // Items accessed twice should be promoted from probation
    // to protected, making them harder to evict.
    CacheStripe stripe(slab, 50, 0.05f, 0.20f);

    // Insert and access "important" key multiple times
    stripe.put("important", "data");
    for (int i = 0; i < 10; ++i) {
        stripe.get("important");
    }

    // Fill the cache with other items
    for (int i = 0; i < 100; ++i) {
        stripe.put("filler_" + std::to_string(i), "v");
    }

    // "important" should survive because it's in protected segment
    auto val = stripe.get("important");
    EXPECT_TRUE(val.has_value())
        << "Protected item evicted — SLRU promotion not working";
}

// ─── Admission/rejection counters ───────────────────────────

TEST_F(CacheStripeTest, AdmissionRejectionCountersWork) {
    CacheStripe stripe(slab, 10);

    // Fill the cache
    for (int i = 0; i < 30; ++i) {
        stripe.put("k" + std::to_string(i), "v");
    }

    // After filling, we should see some admissions and rejections
    EXPECT_GT(stripe.admission_count() + stripe.rejection_count(), 0u);
}

// ─── Hit/miss counters ──────────────────────────────────────

TEST_F(CacheStripeTest, HitMissCountersAccurate) {
    CacheStripe stripe(slab, 100);

    stripe.put("k1", "v1");
    stripe.put("k2", "v2");

    stripe.get("k1");       // hit
    stripe.get("k2");       // hit
    stripe.get("missing");  // miss

    EXPECT_EQ(stripe.hit_count(), 2u);
    EXPECT_EQ(stripe.miss_count(), 1u);
}

// ─── Concurrent correctness ─────────────────────────────────

TEST_F(CacheStripeTest, ConcurrentPutGetNoCorruption) {
    CacheStripe stripe(slab, 256);
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 2000;

    auto worker = [&](int tid) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::string key = "t" + std::to_string(tid) + "_k" + std::to_string(i % 100);
            std::string val = "v" + std::to_string(i);

            if (i % 3 == 0) {
                stripe.put(key, val);
            } else {
                stripe.get(key);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_LE(stripe.size(), 256u);
    EXPECT_GT(stripe.hit_count() + stripe.miss_count(), 0u);
}

// ─── Small capacity edge cases ──────────────────────────────

TEST_F(CacheStripeTest, MinimumCapacityWorks) {
    // Capacity 3: window=1, probation=1, protected=1
    CacheStripe stripe(slab, 3);

    stripe.put("a", "1");
    stripe.put("b", "2");
    stripe.put("c", "3");
    stripe.put("d", "4");

    EXPECT_LE(stripe.size(), 3u);
}

TEST_F(CacheStripeTest, UpdateDoesNotChangeCapacity) {
    CacheStripe stripe(slab, 10);

    for (int i = 0; i < 10; ++i) {
        stripe.put("k" + std::to_string(i), "v1");
    }

    // Update all existing keys — size should not change
    for (int i = 0; i < 10; ++i) {
        stripe.put("k" + std::to_string(i), "v2");
    }

    EXPECT_LE(stripe.size(), 10u);

    // All should have updated value
    for (int i = 0; i < 10; ++i) {
        auto val = stripe.get("k" + std::to_string(i));
        if (val.has_value()) {
            EXPECT_EQ(*val, "v2");
        }
    }
}
