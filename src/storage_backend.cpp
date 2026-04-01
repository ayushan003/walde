#include "storage_backend.h"
#include <mutex>
#include <shared_mutex>
namespace walde {

// ─── InMemoryBackend ────────────────────────────────────────

std::optional<std::string> InMemoryBackend::get(const std::string& key) {
    reads_.fetch_add(1, std::memory_order_relaxed);
    
    // shared_lock allows multiple threads to read concurrently
    std::shared_lock<std::shared_mutex> lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool InMemoryBackend::put(const std::string& key, const std::string& value) {
    writes_.fetch_add(1, std::memory_order_relaxed);
    
    // unique_lock ensures only one thread can write at a time
    std::unique_lock<std::shared_mutex> lock(mtx_);
    store_[key] = value;
    return true;
}

bool InMemoryBackend::remove(const std::string& key) {
    // unique_lock ensures only one thread can modify at a time
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }
    store_.erase(it);
    return true;
}

}  // namespace walde
