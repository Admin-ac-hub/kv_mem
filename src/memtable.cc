#include "memtable.h"

#include <utility>

#include "vector_value.h"

namespace kv {

MemTable::MemTable(std::shared_ptr<HNSWIndex> vector_index)
    : vector_index_(std::move(vector_index)) {}

Status MemTable::Put(std::string key,
                     SequenceNumber sequence,
                     std::string value) {
  if (vector_index_ != nullptr) {
    Status status = vector_index_->MarkDeleted(key, sequence);
    if (!status.ok()) {
      return status;
    }
  }
  table_.Put(std::move(key), sequence, std::move(value));
  return Status::OK();
}

Status MemTable::PutVector(std::string key,
                           SequenceNumber sequence,
                           std::string encoded_value) {
  VectorRecord record;
  Status status = DecodeVectorValue(encoded_value, &record);
  if (!status.ok()) {
    return status;
  }
  if (vector_index_ != nullptr) {
    status = vector_index_->InsertVersion(key, sequence, record.vector,
                                          record.metadata);
    if (!status.ok()) {
      return status;
    }
  }
  table_.Put(std::move(key), sequence, std::move(encoded_value));
  return Status::OK();
}

Status MemTable::Delete(std::string key, SequenceNumber sequence) {
  if (vector_index_ != nullptr) {
    Status status = vector_index_->MarkDeleted(key, sequence);
    if (!status.ok()) {
      return status;
    }
  }
  table_.Delete(std::move(key), sequence);
  return Status::OK();
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

}  // namespace kv
