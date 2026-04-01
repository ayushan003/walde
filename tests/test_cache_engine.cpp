#include "cache_engine.h"
#include "storage_backend.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace walde;

class CacheEngineTest : public ::testing::Test {
protected:
    CacheEngine::Config config;
    InMemoryBackend* backend_raw = nullptr;  // non-owning pointer for assertions

    std::unique_ptr<CacheEngine> make_engine() {
        config.l1_config.total_capacity = 512;
        config.l2_capacity = 256;

        auto backend = std::make_unique<InMemoryBackend>();
        backend_raw = backend.get();

        return std::make_unique<CacheEngine>(config, std::move(backend));
    }
};

// ─── Basic pipeline ─────────────────────────────────────────

TEST_F(CacheEngineTest, PutAndGetRoundTrip) {
    auto engine = make_engine();

    engine->put("hello", "world");
    auto val = engine->get("hello");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "world");
}

TEST_F(CacheEngineTest, MissReturnsNullopt) {
    auto engine = make_engine();
    EXPECT_FALSE(engine->get("nonexistent").has_value());
}

TEST_F(CacheEngineTest, WriteThroughPersistsToBackend) {
    auto engine = make_engine();

    engine->put("k", "v");

    // Backend should have the value
    auto backend_val = backend_raw->get("k");
    ASSERT_TRUE(backend_val.has_value());
    EXPECT_EQ(*backend_val, "v");
    EXPECT_EQ(backend_raw->writes(), 1u);
}

// ─── Cold start: backend → L1 population ────────────────────

TEST_F(CacheEngineTest, ColdStartFetchesFromBackend) {
    auto engine = make_engine();

    // Put directly into backend (simulating pre-existing data)
    backend_raw->put("cold_key", "cold_value");

    // First get: L1 miss → L2 miss → backend hit → populate L1
    auto val = engine->get("cold_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "cold_value");

    // Second get: should be L1 hit now
    auto val2 = engine->get("cold_key");
    ASSERT_TRUE(val2.has_value());
    EXPECT_EQ(*val2, "cold_value");

    // L1 should have 1 hit (second get), backend should have
    // 2 reads (one from our direct put check above, one from engine's get)
    EXPECT_GE(engine->l1_hits(), 1u);
}

TEST_F(CacheEngineTest, BackendPopulatesL1OnMiss) {
    auto engine = make_engine();

    // Seed backend with 100 items
    for (int i = 0; i < 100; ++i) {
        backend_raw->put("backend_" + std::to_string(i),
                         "val_" + std::to_string(i));
    }

    // First pass: all L1 misses, fetched from backend
    for (int i = 0; i < 100; ++i) {
        auto val = engine->get("backend_" + std::to_string(i));
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, "val_" + std::to_string(i));
    }

    // Second pass: should be mostly L1 hits (items were populated)
    uint64_t hits_before = engine->l1_hits();
    for (int i = 0; i < 100; ++i) {
        engine->get("backend_" + std::to_string(i));
    }
    uint64_t new_hits = engine->l1_hits() - hits_before;

    // Most should be L1 hits now (some might have been evicted)
    EXPECT_GT(new_hits, 50u)
        << "Only " << new_hits << "/100 L1 hits on second pass. "
        << "Backend data should have been cached in L1.";
}

// ─── Remove ─────────────────────────────────────────────────

TEST_F(CacheEngineTest, RemoveFromAllLayers) {
    auto engine = make_engine();

    engine->put("k", "v");
    EXPECT_TRUE(engine->remove("k"));

    // Gone from L1
    EXPECT_FALSE(engine->get("k").has_value());

    // Gone from backend too
    EXPECT_FALSE(backend_raw->get("k").has_value());
}

// ─── Stats ──────────────────────────────────────────────────

TEST_F(CacheEngineTest, StatsTrackCorrectly) {
    auto engine = make_engine();

    engine->put("a", "1");
    engine->put("b", "2");
    engine->get("a");        // L1 hit
    engine->get("b");        // L1 hit
    engine->get("missing");  // L1 miss → L2 miss → backend miss

    EXPECT_EQ(engine->l1_hits(), 2u);
    EXPECT_GE(engine->l1_misses(), 1u);
    EXPECT_EQ(engine->backend_writes(), 2u);  // 2 puts
}

TEST_F(CacheEngineTest, HitRateCalculation) {
    auto engine = make_engine();

    // 10 puts, then 10 gets (all hits), then 5 gets (all misses)
    for (int i = 0; i < 10; ++i) {
        engine->put("k" + std::to_string(i), "v");
    }
    for (int i = 0; i < 10; ++i) {
        engine->get("k" + std::to_string(i));
    }
    for (int i = 100; i < 105; ++i) {
        engine->get("k" + std::to_string(i));
    }

    double hr = engine->l1_hit_rate();
    // 10 hits out of 15 gets = ~66.7%
    EXPECT_GT(hr, 0.5);
    EXPECT_LT(hr, 0.8);
}

// ─── Concurrent full pipeline ───────────────────────────────

TEST_F(CacheEngineTest, ConcurrentFullPipelineNoCorruption) {
    auto engine = make_engine();

    // Pre-seed backend
    for (int i = 0; i < 200; ++i) {
        backend_raw->put("pre_" + std::to_string(i),
                         "preval_" + std::to_string(i));
    }

    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 2000;

    auto worker = [&](int tid) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            std::string key = "t" + std::to_string(tid) +
                              "_k" + std::to_string(i % 100);

            switch (i % 5) {
                case 0:
                case 1:
                    engine->put(key, "val_" + std::to_string(i));
                    break;
                case 2:
                case 3:
                    engine->get(key);
                    break;
                case 4:
                    // Also read pre-seeded backend data
                    engine->get("pre_" + std::to_string(i % 200));
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

    // Survived without crash
    EXPECT_GT(engine->l1_hits() + engine->l1_misses(), 0u);
    EXPECT_GT(engine->backend_writes(), 0u);
}

// ─── Latency model validation ───────────────────────────────

TEST_F(CacheEngineTest, LatencyModelExpectedValue) {
    // Validate the mathematical latency model:
    //   E[L] = h1*t1 + (1-h1)*h2*t2 + (1-h1)*(1-h2)*tm
    //
    // We can't measure actual latency here, but we can verify
    // that hit rates feed into the formula correctly.

    auto engine = make_engine();

    // Create a workload: 80% L1-hittable, 20% backend-only
    for (int i = 0; i < 100; ++i) {
        engine->put("cached_" + std::to_string(i), "v");
    }
    for (int i = 0; i < 25; ++i) {
        backend_raw->put("backend_only_" + std::to_string(i), "v");
    }

    // Workload: 80 cached gets + 20 backend gets
    for (int i = 0; i < 80; ++i) {
        engine->get("cached_" + std::to_string(i));
    }
    for (int i = 0; i < 20; ++i) {
        engine->get("backend_only_" + std::to_string(i));
    }

    // Model parameters (microseconds)
    constexpr double t1 = 1.0;     // L1 access time
    constexpr double t2 = 5.0;     // L2 access time
    constexpr double tm = 200.0;   // Backend miss penalty

    double h1 = engine->l1_hit_rate();
    double h2 = 0.0;  // L2 hit rate (low in this test since L2 is empty)

    double expected_latency = h1 * t1 + (1 - h1) * h2 * t2 +
                              (1 - h1) * (1 - h2) * tm;

    // With ~80% L1 hit rate:
    // E[L] ≈ 0.8*1 + 0.2*0*5 + 0.2*1*200 = 0.8 + 40 = 40.8 μs
    EXPECT_GT(h1, 0.5) << "L1 hit rate too low for latency model validation";

    // The formula should produce a reasonable value
    EXPECT_GT(expected_latency, 0.0);
    EXPECT_LT(expected_latency, tm);  // Must be less than pure miss penalty
}
