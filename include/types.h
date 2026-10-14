#pragma once

#include <cstdint>
#include <string>

namespace kv {

using SequenceNumber = std::uint64_t;

// Value-only view of one record: the key is not part of it. Containers keep
// the key themselves (e.g. SkipList::Node::internal_key), so this struct is
// shared both inside the skiplist and across module boundaries
// (MemTable::Get returns it), keeping one definition of "what a version is".
struct Entry {
  SequenceNumber sequence = 0;
  std::string value;
  bool deleted = false;
};

// Full record including its key, used when data leaves its container
// (memtable flush, SSTable creation, WAL replay).
struct VersionedEntry {
  std::string key;
  SequenceNumber sequence = 0;
  std::string value;
  bool deleted = false;
};

}  // namespace kv
