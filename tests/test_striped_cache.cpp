#include "striped_cache.h"

#include <gtest/gtest.h>
#include <cmath>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace walde;

// ─── Basic operations ───────────────────────────────────────

TEST(StripedCache, PutAndGet) {
    StripedCache cache(StripedCache::Config{});

    EXPECT_TRUE(cache.put("hello", "world"));
    auto val = cache.get("hello");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "world");
}

TEST(StripedCache, MissReturnsNullopt) {
    StripedCache cache(StripedCache::Config{});
    EXPECT_FALSE(cache.get("missing").has_value());
}

TEST(StripedCache, UpdateExistingKey) {
    StripedCache cache(StripedCache::Config{});

    cache.put("k", "v1");
    cache.put("k", "v2");

    auto val = cache.get("k");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v2");
}

TEST(StripedCache, RemoveKey) {
    StripedCache cache(StripedCache::Config{});

    cache.put("k", "v");
    EXPECT_TRUE(cache.remove("k"));
    EXPECT_FALSE(cache.get("k").has_value());
    EXPECT_FALSE(cache.remove("k"));  // already gone
}

// ─── Batch get ──────────────────────────────────────────────

TEST(StripedCache, BatchGetParallelResults) {
    StripedCache cache(StripedCache::Config{});

    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    auto results = cache.batch_get({"a", "missing", "c"});
    ASSERT_EQ(results.size(), 3u);

    ASSERT_TRUE(results[0].has_value());
    EXPECT_EQ(*results[0], "1");

    EXPECT_FALSE(results[1].has_value());

    ASSERT_TRUE(results[2].has_value());
    EXPECT_EQ(*results[2], "3");
}

// ─── Stripe distribution ────────────────────────────────────

TEST(StripedCache, KeysDistributeAcrossStripes) {
    // Insert many keys and verify they spread across stripes.
    // With XXHash and 64 stripes, distribution should be near-uniform.
    StripedCache::Config config;
    config.total_capacity = 8192;
    StripedCache cache(config);

    for (int i = 0; i < 4000; ++i) {
        cache.put("key_" + std::to_string(i), "val");
    }

    // Check that multiple stripes have hits/misses (not all concentrated)
    // FIXED: Supress unused variable warning
    [[maybe_unused]] int active_stripes = 0;
    for (uint32_t s = 0; s < walde::kNumStripes; ++s) {
        // After inserts, some stripes should have seen put operations
        // We can't directly check stripe sizes, but we can verify
        // that gets distribute across stripes
    }

    // Do gets on all keys — misses should also distribute
    for (int i = 0; i < 4000; ++i) {
        cache.get("key_" + std::to_string(i));
    }

    // Verify aggregate stats make sense
    uint64_t hits = cache.total_hits();
    
    // FIXED: Supress unused variable warning
    [[maybe_unused]] uint64_t misses = cache.total_misses();
    EXPECT_GT(hits, 0u);

    // Check per-stripe distribution: no single stripe should hold
    // more than 5% of all hits (expected ~1.5% each for uniform)
    for (uint32_t s = 0; s < walde::kNumStripes; ++s) {
        uint64_t sh = cache.stripe_hits(s);
        if (hits > 0) {
            double fraction = static_cast<double>(sh) / hits;
            EXPECT_LT(fraction, 0.05)
                << "Stripe " << s << " has " << (fraction * 100)
                << "% of all hits — distribution is skewed";
        }
    }
}

// ─── Capacity management ────────────────────────────────────

TEST(StripedCache, DoesNotExceedCapacity) {
    StripedCache::Config config;
    config.total_capacity = 1024;
    StripedCache cache(config);

    for (int i = 0; i < 5000; ++i) {
        cache.put("k" + std::to_string(i), "v");
    }

    EXPECT_LE(cache.total_size(), 1024u);
}

// ─── Aggregate stats ────────────────────────────────────────

TEST(StripedCache, AggregateStatsConsistent) {
    StripedCache::Config config;
    config.total_capacity = 512;
    StripedCache cache(config);

    for (int i = 0; i < 200; ++i) {
        cache.put("k" + std::to_string(i), "v");
    }
    for (int i = 0; i < 300; ++i) {
        cache.get("k" + std::to_string(i));  // 200 hits, 100 misses
    }

    EXPECT_EQ(cache.total_hits() + cache.total_misses(), 300u);
    EXPECT_EQ(cache.total_hits(), 200u);
    EXPECT_EQ(cache.total_misses(), 100u);
}

// ─── Multi-threaded correctness ─────────────────────────────

TEST(StripedCache, ConcurrentPutGetNoCorruption) {
    StripedCache::Config config;
    config.total_capacity = 4096;
    StripedCache cache(config);

    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 5000;

    auto worker = [&](int tid) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::string key = "t" + std::to_string(tid) +
                              "_k" + std::to_string(i % 200);
            std::string val = "v" + std::to_string(i);

            switch (i % 4) {
                case 0:
                case 1:
                    cache.put(key, val);
                    break;
                case 2:
                    cache.get(key);
                    break;
                case 3:
                    cache.remove(key);
                    break;
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

    // No crashes, no assertions, size within bounds
    EXPECT_LE(cache.total_size(), 4096u);

    uint64_t total_ops = cache.total_hits() + cache.total_misses();
    EXPECT_GT(total_ops, 0u);
}

TEST(StripedCache, ConcurrentSameKeysConsistent) {
    // Multiple threads writing to the SAME keys — tests lock correctness
    StripedCache::Config config;
    config.total_capacity = 1024;
    StripedCache cache(config);

    constexpr int kThreads = 8;
    constexpr int kKeys = 50;

    // Pre-populate
    for (int i = 0; i < kKeys; ++i) {
        cache.put("shared_" + std::to_string(i), "init");
    }

    auto writer = [&](int tid) {
        for (int round = 0; round < 500; ++round) {
            for (int i = 0; i < kKeys; ++i) {
                std::string key = "shared_" + std::to_string(i);
                cache.put(key, "t" + std::to_string(tid) + "_r" + std::to_string(round));
            }
        }
    };

    auto reader = [&]() {
        for (int round = 0; round < 500; ++round) {
            for (int i = 0; i < kKeys; ++i) {
                auto val = cache.get("shared_" + std::to_string(i));
                // Value may or may not exist (could be evicted), but
                // if it exists it must be a valid string, not garbage
                if (val.has_value()) {
                    EXPECT_FALSE(val->empty());
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads / 2; ++t) {
        threads.emplace_back(writer, t);
    }
    for (int t = 0; t < kThreads / 2; ++t) {
        threads.emplace_back(reader);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_LE(cache.total_size(), 1024u);
}

// ─── Scan resistance at scale ───────────────────────────────

TEST(StripedCache, ScanResistanceAtScale) {
    StripedCache::Config config;
    config.total_capacity = 4096;
    StripedCache cache(config);

    // Phase 1: Build hot working set of 500 keys, each accessed 10+ times
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 500; ++i) {
            cache.put("hot_" + std::to_string(i), "important_data");
        }
    }
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 500; ++i) {
            cache.get("hot_" + std::to_string(i));
        }
    }

    // Phase 2: Scan attack — 10,000 unique one-shot keys
    for (int i = 0; i < 10000; ++i) {
        cache.put("scan_" + std::to_string(i), "junk");
    }

    // Phase 3: Measure hot key survival
    int hot_survived = 0;
    for (int i = 0; i < 500; ++i) {
        if (cache.get("hot_" + std::to_string(i)).has_value()) {
            ++hot_survived;
        }
    }

    double survival_rate = static_cast<double>(hot_survived) / 500.0;

    // With W-TinyLFU across 64 stripes, hot keys should survive well.
    // We expect at least 40% survival. Pure LRU: 0%.
    EXPECT_GE(hot_survived, 200)
        << "Only " << hot_survived << "/500 (" << (survival_rate * 100)
        << "%) hot keys survived. W-TinyLFU should protect the working set.";

    // Verify the gate was active
    EXPECT_GT(cache.total_rejections(), 0u);
}
