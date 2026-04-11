#pragma once

#include <cstdint>
#include <string>

namespace walde {

// ─── Constants ──────────────────────────────────────────────
constexpr uint32_t kNumStripes   = 64;
constexpr uint32_t kInvalidIndex = UINT32_MAX;

// ─── CacheNode ──────────────────────────────────────────────
struct CacheNode {
    uint32_t prev = kInvalidIndex;
    uint32_t next = kInvalidIndex;
    std::string key;
    std::string value;
    uint8_t segment = 0;

    void reset() {
        prev    = kInvalidIndex;
        next    = kInvalidIndex;
        key.clear();
        value.clear();
        segment = 0;
    }
};

enum class Segment : uint8_t {
    Free      = 0,
    Window    = 1,
    Probation = 2,
    Protected = 3,
    L2        = 4,
};

}  // namespace walde
