#include "count_min_sketch.h"

#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <unordered_map>

using namespace walde;

// ─── Basic operations ───────────────────────────────────────

TEST(CountMinSketch, FreshSketchEstimatesZero) {
    CountMinSketch cms(4, 2048);
    EXPECT_EQ(cms.estimate("anything"), 0u);
    EXPECT_EQ(cms.estimate(""), 0u);
}

TEST(CountMinSketch, SingleIncrementEstimatesOne) {
    CountMinSketch cms(4, 2048);
    cms.increment("hello");
    EXPECT_EQ(cms.estimate("hello"), 1u);
}

TEST(CountMinSketch, MultipleIncrementsAccurate) {
    CountMinSketch cms(4, 2048);

    for (int i = 0; i < 100; ++i) {
        cms.increment("hot_key");
    }

    // With 4 rows × 2048 width, collisions on a single key are
    // extremely unlikely. Should be exactly 100.
    EXPECT_EQ(cms.estimate("hot_key"), 100u);
}

TEST(CountMinSketch, DifferentKeysIndependent) {
    CountMinSketch cms(4, 2048);

    cms.increment("alpha");
    cms.increment("alpha");
    cms.increment("alpha");
    cms.increment("beta");

    EXPECT_EQ(cms.estimate("alpha"), 3u);
    EXPECT_EQ(cms.estimate("beta"), 1u);
}

TEST(CountMinSketch, TotalIncrementsTracked) {
    CountMinSketch cms(4, 2048);

    for (int i = 0; i < 50; ++i) {
        cms.increment("k" + std::to_string(i));
    }

    EXPECT_EQ(cms.total_increments(), 50u);
}

// ─── Overestimation property ────────────────────────────────

TEST(CountMinSketch, NeverUnderestimates) {
    // CMS guarantee: estimate(key) >= true_count(key)
    // It may overestimate due to hash collisions, but never underestimate.
    CountMinSketch cms(4, 1024);  // Smaller width to increase collision chance

    std::unordered_map<std::string, uint32_t> true_counts;

    for (int i = 0; i < 5000; ++i) {
        std::string key = "key_" + std::to_string(i % 200);
        cms.increment(key);
        true_counts[key]++;
    }

    for (auto& [key, true_count] : true_counts) {
        uint32_t estimated = cms.estimate(key);
        EXPECT_GE(estimated, true_count)
            << "CMS underestimated " << key
            << ": estimated=" << estimated
            << " true=" << true_count;
    }
}

TEST(CountMinSketch, OverestimationBounded) {
    // With depth=4 and width=2048, overestimation should be small
    // for a reasonable number of unique keys.
    CountMinSketch cms(4, 2048);

    // Insert 500 unique keys, each with a known frequency
    std::unordered_map<std::string, uint32_t> true_counts;
    for (int i = 0; i < 500; ++i) {
        std::string key = "item_" + std::to_string(i);
        uint32_t freq = (i % 10) + 1;  // Frequencies 1–10
        for (uint32_t f = 0; f < freq; ++f) {
            cms.increment(key);
        }
        true_counts[key] = freq;
    }

    // Check that most estimates are exact or very close
    uint32_t exact = 0;
    uint32_t within_1 = 0;
    for (auto& [key, true_count] : true_counts) {
        uint32_t est = cms.estimate(key);
        if (est == true_count) ++exact;
        if (est <= true_count + 1) ++within_1;
    }

    // With these parameters, vast majority should be exact
    EXPECT_GT(exact, 450u)
        << "Too many collisions: only " << exact << "/500 exact";
    EXPECT_GT(within_1, 490u);
}

// ─── Decay ──────────────────────────────────────────────────

TEST(CountMinSketch, DecayHalvesCounters) {
    CountMinSketch cms(4, 2048);

    for (int i = 0; i < 100; ++i) {
        cms.increment("key");
    }
    EXPECT_EQ(cms.estimate("key"), 100u);

    cms.decay();
    EXPECT_EQ(cms.estimate("key"), 50u);

    cms.decay();
    EXPECT_EQ(cms.estimate("key"), 25u);
}

TEST(CountMinSketch, DecayAdaptsToWorkloadShift) {
    // Simulate a workload shift: "old_hot" was popular,
    // then "new_hot" becomes popular. After decay, new_hot
    // should dominate.
    CountMinSketch cms(4, 2048);

    // Phase 1: old_hot is heavily accessed
    for (int i = 0; i < 200; ++i) {
        cms.increment("old_hot");
    }

    // Decay to reduce old_hot's influence
    cms.decay();  // 200 → 100
    cms.decay();  // 100 → 50

    // Phase 2: new_hot gets moderate access
    for (int i = 0; i < 80; ++i) {
        cms.increment("new_hot");
    }

    // new_hot (80) should now beat old_hot (50)
    EXPECT_GT(cms.estimate("new_hot"), cms.estimate("old_hot"));
}

TEST(CountMinSketch, DecayOnZeroCountersIsHarmless) {
    CountMinSketch cms(4, 2048);
    cms.decay();  // No crash, no UB
    EXPECT_EQ(cms.estimate("anything"), 0u);
}

// ─── Reset ──────────────────────────────────────────────────

TEST(CountMinSketch, ResetClearsEverything) {
    CountMinSketch cms(4, 2048);

    for (int i = 0; i < 100; ++i) {
        cms.increment("key_" + std::to_string(i));
    }

    cms.reset();

    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(cms.estimate("key_" + std::to_string(i)), 0u);
    }
    EXPECT_EQ(cms.total_increments(), 0u);
}

// ─── Determinism ────────────────────────────────────────────

TEST(CountMinSketch, DeterministicAcrossInstances) {
    // Two CMS instances with same parameters should produce
    // identical results for the same sequence of operations.
    CountMinSketch a(4, 2048);
    CountMinSketch b(4, 2048);

    for (int i = 0; i < 50; ++i) {
        std::string key = "det_" + std::to_string(i);
        a.increment(key);
        b.increment(key);
    }

    for (int i = 0; i < 50; ++i) {
        std::string key = "det_" + std::to_string(i);
        EXPECT_EQ(a.estimate(key), b.estimate(key));
    }
}

// ─── Edge cases ─────────────────────────────────────────────

TEST(CountMinSketch, EmptyKeyWorks) {
    CountMinSketch cms(4, 2048);
    cms.increment("");
    cms.increment("");
    EXPECT_EQ(cms.estimate(""), 2u);
}

TEST(CountMinSketch, VeryLongKeyWorks) {
    CountMinSketch cms(4, 2048);
    std::string long_key(10000, 'x');
    cms.increment(long_key);
    EXPECT_EQ(cms.estimate(long_key), 1u);
}
