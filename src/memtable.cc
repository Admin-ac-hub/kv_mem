#include "memtable.h"

namespace kv {

void MemTable::Put(std::string key, SequenceNumber sequence, std::string value) {
  table_.Put(std::move(key), sequence, std::move(value));
}

void MemTable::Delete(std::string key, SequenceNumber sequence) {
  table_.Delete(std::move(key), sequence);
}

std::optional<SkipList::Entry> MemTable::Get(const std::string& key,
                                             SequenceNumber read_sequence) const {
  return table_.Get(key, read_sequence);
}

std::vector<VersionedEntry> MemTable::Entries() const {
  return table_.Entries();
}

size_t MemTable::Size() const {
  return table_.Size();
}

void MemTable::Clear() {
  table_.Clear();
}

}  // namespace kv
