#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "status.h"
#include "types.h"

namespace kv {

// Forward declaration: memtable.h deliberately does not include skiplist.h so
// that users of MemTable (DB included) never depend on the skiplist internals.
class SkipList;

class MemTable {
 public:
  MemTable();
  ~MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  Status Put(std::string key, SequenceNumber sequence, std::string value);
  Status Delete(std::string key, SequenceNumber sequence);
  std::optional<Entry> Get(const std::string& key,
                           SequenceNumber read_sequence) const;
  std::vector<VersionedEntry> Entries() const;
  size_t Size() const;

 private:
  std::unique_ptr<SkipList> table_;
};

}  // namespace kv
