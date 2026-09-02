#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hnsw_index.h"
#include "skiplist.h"
#include "status.h"

namespace kv {

class MemTable {
 public:
  explicit MemTable(std::shared_ptr<HNSWIndex> vector_index = nullptr);

  Status Put(std::string key, SequenceNumber sequence, std::string value);
  Status PutVector(std::string key,
                   SequenceNumber sequence,
                   std::string encoded_value);
  Status Delete(std::string key, SequenceNumber sequence);
  std::optional<SkipList::Entry> Get(const std::string& key,
                                     SequenceNumber read_sequence) const;
  std::vector<VersionedEntry> Entries() const;
  size_t Size() const;

 private:
  SkipList table_;
  std::shared_ptr<HNSWIndex> vector_index_;
};

}  // namespace kv
