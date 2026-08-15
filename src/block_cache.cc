#include "block_cache.h"

#include <utility>

namespace kv {

BlockCache::BlockCache(size_t capacity) : capacity_(capacity) {}

bool BlockCache::Get(std::uint64_t file_number, std::uint64_t block_offset, std::string* block) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ == 0) {
    ++stats_.misses;
    return false;
  }

  Key key{file_number, block_offset};
  auto it = index_.find(key);
  if (it == index_.end()) {
    ++stats_.misses;
    return false;
  }

  entries_.splice(entries_.begin(), entries_, it->second);
  *block = it->second->second;
  ++stats_.hits;
  return true;
}

void BlockCache::Put(std::uint64_t file_number, std::uint64_t block_offset, std::string block) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ == 0) {
    return;
  }

  Key key{file_number, block_offset};
  auto it = index_.find(key);
  if (it != index_.end()) {
    it->second->second = std::move(block);
    entries_.splice(entries_.begin(), entries_, it->second);
    return;
  }

  entries_.push_front({key, std::move(block)});
  index_[key] = entries_.begin();
  while (entries_.size() > capacity_) {
    index_.erase(entries_.back().first);
    entries_.pop_back();
  }
}

BlockCacheStats BlockCache::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace kv
