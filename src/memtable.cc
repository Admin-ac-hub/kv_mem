#include "memtable.h"

#include <utility>

#include "skiplist.h"

namespace kv {

MemTable::MemTable() : table_(std::make_unique<SkipList>()) {}

MemTable::~MemTable() = default;

Status MemTable::Put(std::string key,
                     SequenceNumber sequence,
                     std::string value) {
  table_->Put(std::move(key), sequence, std::move(value));
  return Status::OK();
}

Status MemTable::Delete(std::string key, SequenceNumber sequence) {
  table_->Delete(std::move(key), sequence);
  return Status::OK();
}

std::optional<Entry> MemTable::Get(const std::string& key,
                                   SequenceNumber read_sequence) const {
  return table_->Get(key, read_sequence);
}

std::vector<VersionedEntry> MemTable::Entries() const {
  return table_->Entries();
}

size_t MemTable::Size() const {
  return table_->Size();
}

}  // namespace kv
