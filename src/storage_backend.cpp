#include "storage_backend.h"
#include <mutex>
#include <shared_mutex>

namespace walde {

std::optional<std::string> InMemoryBackend::get(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    reads_.fetch_add(1, std::memory_order_relaxed);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool InMemoryBackend::put(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    writes_.fetch_add(1, std::memory_order_relaxed);
    store_[key] = value;
    return true;
}

bool InMemoryBackend::remove(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }
    store_.erase(it);
    return true;
}

}  // namespace walde
