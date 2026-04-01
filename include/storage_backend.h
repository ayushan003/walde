#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace walde {

// ─── StorageBackend ─────────────────────────────────────────
//
// Abstract interface for the backing store beneath L1/L2.
// In production this would wrap RocksDB or similar.
// In benchmarks/tests, InMemoryBackend is used.

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual std::optional<std::string> get(const std::string& key)                         = 0;
    virtual bool                       put(const std::string& key, const std::string& value) = 0;
    virtual bool                       remove(const std::string& key)                        = 0;

    // Optional stat accessors — implementations may return 0 if not tracked.
    virtual uint64_t reads()  const { return 0; }
    virtual uint64_t writes() const { return 0; }
};

// ─── InMemoryBackend ────────────────────────────────────────
//
// Thread-safe in-memory key-value store backed by unordered_map.
// Uses shared_mutex for concurrent reads / exclusive writes.
// Used in benchmarks to isolate cache behaviour from real disk I/O.

class InMemoryBackend : public StorageBackend {
public:
    std::optional<std::string> get(const std::string& key)                         override;
    bool                       put(const std::string& key, const std::string& value) override;
    bool                       remove(const std::string& key)                        override;

    uint64_t reads()  const override { return reads_.load(std::memory_order_relaxed); }
    uint64_t writes() const override { return writes_.load(std::memory_order_relaxed); }

private:
    mutable std::shared_mutex                    mtx_;
    std::unordered_map<std::string, std::string> store_;
    std::atomic<uint64_t>                        reads_  {0};
    std::atomic<uint64_t>                        writes_ {0};
};

}  // namespace walde
