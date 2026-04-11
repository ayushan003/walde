#include "lru_list.h"
#include "slab_allocator.h"

#include <gtest/gtest.h>
#include <vector>

using namespace walde;

class LRUListTest : public ::testing::Test {
protected:
    static constexpr uint32_t kSlabSize = 64;
    SlabAllocator slab{kSlabSize};
    IntrusiveLRU lru{slab};

    // Helper: allocate a node with a key and push to front
    uint32_t alloc_and_push(const std::string& key) {
        uint32_t idx = slab.allocate();
        EXPECT_NE(idx, kInvalidIndex);
        slab.node(idx).key = key;
        lru.push_front(idx);
        return idx;
    }

    // Helper: collect all keys from MRU to LRU by walking the list
    std::vector<std::string> collect_keys() {
        std::vector<std::string> keys;
        uint32_t cur = lru.peek_front();
        while (cur != kInvalidIndex) {
            keys.push_back(slab.node(cur).key);
            cur = slab.node(cur).next;
        }
        return keys;
    }
};

// ─── Empty list ─────────────────────────────────────────────

TEST_F(LRUListTest, EmptyListState) {
    EXPECT_TRUE(lru.empty());
    EXPECT_EQ(lru.size(), 0u);
    EXPECT_EQ(lru.peek_front(), kInvalidIndex);
    EXPECT_EQ(lru.peek_back(), kInvalidIndex);
    EXPECT_EQ(lru.pop_back(), kInvalidIndex);
}

// ─── Single element ─────────────────────────────────────────

TEST_F(LRUListTest, SingleElement) {
    uint32_t a = alloc_and_push("alpha");

    EXPECT_EQ(lru.size(), 1u);
    EXPECT_EQ(lru.peek_front(), a);
    EXPECT_EQ(lru.peek_back(), a);

    // Pop it
    EXPECT_EQ(lru.pop_back(), a);
    EXPECT_TRUE(lru.empty());
}

// ─── Ordering ───────────────────────────────────────────────

TEST_F(LRUListTest, PushFrontMaintainsMRUOrder) {
    // Push A, B, C → list should be C(MRU) → B → A(LRU)
    alloc_and_push("A");
    alloc_and_push("B");
    uint32_t c = alloc_and_push("C");

    EXPECT_EQ(lru.size(), 3u);
    EXPECT_EQ(lru.peek_front(), c);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "C");
    EXPECT_EQ(keys[1], "B");
    EXPECT_EQ(keys[2], "A");
}

TEST_F(LRUListTest, PopBackRemovesLRU) {
    alloc_and_push("A");
    alloc_and_push("B");
    alloc_and_push("C");

    // Pop should return A (LRU)
    uint32_t victim = lru.pop_back();
    EXPECT_EQ(slab.node(victim).key, "A");
    EXPECT_EQ(lru.size(), 2u);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "C");
    EXPECT_EQ(keys[1], "B");
}

// ─── Remove from middle ─────────────────────────────────────

TEST_F(LRUListTest, RemoveFromMiddle) {
    alloc_and_push("A");
    uint32_t b = alloc_and_push("B");
    alloc_and_push("C");

    // Remove B (middle node)
    lru.remove(b);
    EXPECT_EQ(lru.size(), 2u);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "C");
    EXPECT_EQ(keys[1], "A");
}

TEST_F(LRUListTest, RemoveHead) {
    alloc_and_push("A");
    alloc_and_push("B");
    uint32_t c = alloc_and_push("C");

    lru.remove(c);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "B");
    EXPECT_EQ(keys[1], "A");
}

TEST_F(LRUListTest, RemoveTail) {
    uint32_t a = alloc_and_push("A");
    alloc_and_push("B");
    alloc_and_push("C");

    lru.remove(a);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "C");
    EXPECT_EQ(keys[1], "B");
}

// ─── Move to front ──────────────────────────────────────────

TEST_F(LRUListTest, MoveToFrontFromTail) {
    uint32_t a = alloc_and_push("A");
    alloc_and_push("B");
    alloc_and_push("C");

    // A is at LRU position. Move it to MRU.
    lru.move_to_front(a);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "A");  // now MRU
    EXPECT_EQ(keys[1], "C");
    EXPECT_EQ(keys[2], "B");  // now LRU
}

TEST_F(LRUListTest, MoveToFrontFromMiddle) {
    alloc_and_push("A");
    uint32_t b = alloc_and_push("B");
    alloc_and_push("C");

    lru.move_to_front(b);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "B");
    EXPECT_EQ(keys[1], "C");
    EXPECT_EQ(keys[2], "A");
}

TEST_F(LRUListTest, MoveToFrontWhenAlreadyFront) {
    alloc_and_push("A");
    alloc_and_push("B");
    uint32_t c = alloc_and_push("C");

    // C is already front — should be a no-op
    lru.move_to_front(c);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "C");
    EXPECT_EQ(keys[1], "B");
    EXPECT_EQ(keys[2], "A");
    EXPECT_EQ(lru.size(), 3u);
}

// ─── Two elements (edge case for doubly linked) ─────────────

TEST_F(LRUListTest, TwoElementsMoveToFront) {
    uint32_t a = alloc_and_push("A");
    alloc_and_push("B");

    // Move A (tail) to front
    lru.move_to_front(a);

    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "A");
    EXPECT_EQ(keys[1], "B");
}

TEST_F(LRUListTest, TwoElementsPopBothSequentially) {
    alloc_and_push("A");
    alloc_and_push("B");

    uint32_t v1 = lru.pop_back();
    EXPECT_EQ(slab.node(v1).key, "A");

    uint32_t v2 = lru.pop_back();
    EXPECT_EQ(slab.node(v2).key, "B");

    EXPECT_TRUE(lru.empty());
    EXPECT_EQ(lru.pop_back(), kInvalidIndex);
}

// ─── Drain and refill ────────────────────────────────────────

TEST_F(LRUListTest, DrainAndRefillConsistency) {
    // Fill with 10 items
    std::vector<uint32_t> indices;
    for (int i = 0; i < 10; ++i) {
        indices.push_back(alloc_and_push("k" + std::to_string(i)));
    }
    EXPECT_EQ(lru.size(), 10u);

    // Drain completely
    for (int i = 0; i < 10; ++i) {
        uint32_t v = lru.pop_back();
        EXPECT_NE(v, kInvalidIndex);
        slab.deallocate(v);
    }
    EXPECT_TRUE(lru.empty());

    // Refill with 5 items
    for (int i = 0; i < 5; ++i) {
        alloc_and_push("new" + std::to_string(i));
    }
    EXPECT_EQ(lru.size(), 5u);

    // Verify order: new4(MRU) → new3 → new2 → new1 → new0(LRU)
    auto keys = collect_keys();
    ASSERT_EQ(keys.size(), 5u);
    EXPECT_EQ(keys[0], "new4");
    EXPECT_EQ(keys[4], "new0");
}

// ─── Backward traversal (verify prev links) ─────────────────

TEST_F(LRUListTest, BackwardTraversalMatchesForward) {
    for (int i = 0; i < 6; ++i) {
        alloc_and_push("k" + std::to_string(i));
    }

    // Forward: MRU → LRU
    auto forward_keys = collect_keys();

    // Backward: LRU → MRU
    std::vector<std::string> backward_keys;
    uint32_t cur = lru.peek_back();
    while (cur != kInvalidIndex) {
        backward_keys.push_back(slab.node(cur).key);
        cur = slab.node(cur).prev;
    }

    ASSERT_EQ(forward_keys.size(), backward_keys.size());

    // backward should be reverse of forward
    for (size_t i = 0; i < forward_keys.size(); ++i) {
        EXPECT_EQ(forward_keys[i],
                  backward_keys[backward_keys.size() - 1 - i]);
    }
}
