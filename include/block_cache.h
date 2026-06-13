#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace kv {

struct BlockCacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
};

class BlockCache {
 public:
  explicit BlockCache(size_t capacity);

  bool Get(std::uint64_t file_number, std::uint64_t block_offset, std::string* block);
  void Put(std::uint64_t file_number, std::uint64_t block_offset, std::string block);
  BlockCacheStats Stats() const;

 private:
  struct Key {
    std::uint64_t file_number = 0;
    std::uint64_t block_offset = 0;

    bool operator==(const Key& other) const {
      return file_number == other.file_number && block_offset == other.block_offset;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& key) const {
      return std::hash<std::uint64_t>{}(key.file_number) ^
             (std::hash<std::uint64_t>{}(key.block_offset) << 1);
    }
  };

  using EntryList = std::list<std::pair<Key, std::string>>;

  size_t capacity_;
  EntryList entries_;
  std::unordered_map<Key, EntryList::iterator, KeyHash> index_;
  BlockCacheStats stats_;
};

}  // namespace kv
