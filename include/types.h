#pragma once

#include <cstdint>
#include <string>

namespace walde {

// ─── Constants ──────────────────────────────────────────────
constexpr uint32_t kNumStripes   = 64;
constexpr uint32_t kInvalidIndex = UINT32_MAX;

// ─── CacheNode ──────────────────────────────────────────────
// Stored contiguously in the slab allocator's pre-allocated pool.
// LRU linkage uses integer indices into the slab, NOT pointers.
//
// Design: intrusive linked list avoids separate heap allocation
// for list nodes. prev/next are slab indices, not raw pointers.
//
// Size target: ≤ 128 bytes (two per cache line on most CPUs).

struct CacheNode {
    // ── Intrusive LRU linkage (indices into slab) ───────────
    uint32_t prev = kInvalidIndex;
    uint32_t next = kInvalidIndex;

    // ── Key-value payload ───────────────────────────────────
    std::string key;
    std::string value;

    // ── Metadata ────────────────────────────────────────────
    uint8_t segment = 0;  // Segment enum value

    void reset() {
        prev    = kInvalidIndex;
        next    = kInvalidIndex;
        key.clear();
        value.clear();
        segment = 0;
    }
};

// Segment tags (avoid magic numbers in switch statements)
enum class Segment : uint8_t {
    Free      = 0,
    Window    = 1,
    Probation = 2,
    Protected = 3,
    L2        = 4,
};

}  // namespace walde
