#include "skiplist.h"

#include <utility>

#include "format.h"

namespace kv {

SkipList::SkipList()
    : head_("", Entry{}, kMaxLevel),
      level_(1),
      rng_(std::random_device{}()) {}

SkipList::~SkipList() {
  Clear();
}

void SkipList::Clear() {
  Node* node = head_.next[0];
  while (node != nullptr) {
    Node* next = node->next[0];
    delete node;
    node = next;
  }
  for (Node*& next : head_.next) {
    next = nullptr;
  }
  level_ = 1;
  size_ = 0;
}

int SkipList::RandomLevel() {
  int level = 1;
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  while (level < kMaxLevel && dist(rng_) < kBranching) {
    ++level;
  }
  return level;
}

SkipList::Node* SkipList::FindGreaterOrEqual(const std::string& internal_key,
                                             std::vector<Node*>* prev) const {
  Node* current = const_cast<Node*>(&head_);
  for (int i = level_ - 1; i >= 0; --i) {
    while (current->next[static_cast<size_t>(i)] != nullptr &&
           current->next[static_cast<size_t>(i)]->internal_key < internal_key) {
      current = current->next[static_cast<size_t>(i)];
    }
    if (prev != nullptr) {
      (*prev)[static_cast<size_t>(i)] = current;
    }
  }
  return current->next[0];
}

void SkipList::Put(std::string key, SequenceNumber sequence, std::string value) {
  Insert(std::move(key), sequence, std::move(value), false);
}

void SkipList::Delete(std::string key, SequenceNumber sequence) {
  Insert(std::move(key), sequence, "", true);
}

void SkipList::Insert(std::string key,
                      SequenceNumber sequence,
                      std::string value,
                      bool deleted) {
  std::string internal_key = EncodeInternalKey(key, sequence);
  std::vector<Node*> prev(static_cast<size_t>(kMaxLevel), nullptr);
  Node* candidate = FindGreaterOrEqual(internal_key, &prev);
  if (candidate != nullptr && candidate->internal_key == internal_key) {
    candidate->entry.value = std::move(value);
    candidate->entry.sequence = sequence;
    candidate->entry.deleted = deleted;
    return;
  }

  int new_level = RandomLevel();
  if (new_level > level_) {
    for (int i = level_; i < new_level; ++i) {
      prev[static_cast<size_t>(i)] = &head_;
    }
    level_ = new_level;
  }

  Node* node =
      new Node(std::move(internal_key), Entry{sequence, std::move(value), deleted}, new_level);
  for (int i = 0; i < new_level; ++i) {
    node->next[static_cast<size_t>(i)] = prev[static_cast<size_t>(i)]->next[static_cast<size_t>(i)];
    prev[static_cast<size_t>(i)]->next[static_cast<size_t>(i)] = node;
  }
  ++size_;
}

std::optional<SkipList::Entry> SkipList::Get(const std::string& key,
                                             SequenceNumber read_sequence) const {
  Node* candidate = FindGreaterOrEqual(EncodeInternalKey(key, read_sequence), nullptr);
  if (candidate == nullptr) {
    return std::nullopt;
  }
  std::string candidate_key;
  SequenceNumber candidate_sequence = 0;
  if (!DecodeInternalKey(candidate->internal_key, &candidate_key, &candidate_sequence) ||
      candidate_key != key ||
      candidate_sequence > read_sequence) {
    return std::nullopt;
  }
  return candidate->entry;
}

std::vector<VersionedEntry> SkipList::Entries() const {
  std::vector<VersionedEntry> entries;
  entries.reserve(size_);
  Node* node = head_.next[0];
  while (node != nullptr) {
    std::string user_key;
    SequenceNumber sequence = 0;
    if (DecodeInternalKey(node->internal_key, &user_key, &sequence)) {
      entries.push_back(VersionedEntry{std::move(user_key),
                                       sequence,
                                       node->entry.value,
                                       node->entry.deleted});
    }
    node = node->next[0];
  }
  return entries;
}

size_t SkipList::Size() const {
  return size_;
}

}  // namespace kv
