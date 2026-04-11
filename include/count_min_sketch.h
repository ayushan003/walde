#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace walde {

class CountMinSketch {
public:
    CountMinSketch(uint32_t depth, uint32_t width);

    void increment(const std::string& key);
    uint32_t estimate(const std::string& key) const;
    void decay();
    void reset();
    uint64_t total_increments() const { return total_increments_; }

private:
    uint32_t hash(const std::string& key, uint32_t row) const;

    uint32_t              depth_;
    uint32_t              width_;
    std::vector<uint32_t> counters_;
    std::vector<uint32_t> seeds_;
    uint64_t              total_increments_ = 0;
};

}  // namespace walde
