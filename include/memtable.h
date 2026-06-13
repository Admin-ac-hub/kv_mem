#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "skiplist.h"

namespace kv {

class MemTable {
 public:
  void Put(std::string key, SequenceNumber sequence, std::string value);
  void Delete(std::string key, SequenceNumber sequence);
  std::optional<SkipList::Entry> Get(const std::string& key,
                                     SequenceNumber read_sequence) const;
  std::vector<VersionedEntry> Entries() const;
  size_t Size() const;
  void Clear();

 private:
  SkipList table_;
};

}  // namespace kv
