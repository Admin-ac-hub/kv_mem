#include "skiplist.h"

#include <string_view>
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
  Node* node = head_.next[0].load(std::memory_order_relaxed);
  while (node != nullptr) {
    Node* next = node->next[0].load(std::memory_order_relaxed);
    delete node;
    node = next;
  }
  for (auto& next : head_.next) {
    next.store(nullptr, std::memory_order_relaxed);
  }
  retired_nodes_.clear();
  level_.store(1, std::memory_order_relaxed);
  size_.store(0, std::memory_order_relaxed);
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
  Node* next = nullptr;
  for (int i = level_.load(std::memory_order_relaxed) - 1; i >= 0; --i) {
    next = current->next[static_cast<size_t>(i)].load(std::memory_order_acquire);
    while (next != nullptr && next->internal_key < internal_key) {
      current = next;
      next = current->next[static_cast<size_t>(i)].load(std::memory_order_acquire);
    }
    if (prev != nullptr) {
      (*prev)[static_cast<size_t>(i)] = current;
    }
  }
  // Return the examined successor: a reload could observe a new node inserted
  // before the target after the comparison, exposing a too-new sequence.
  return next;
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
  const bool replacing = candidate != nullptr && candidate->internal_key == internal_key;
  const int new_level = replacing ? static_cast<int>(candidate->next.size()) : RandomLevel();
  const int old_level = level_.load(std::memory_order_relaxed);
  if (new_level > old_level) {
    for (int i = old_level; i < new_level; ++i) {
      prev[static_cast<size_t>(i)] = &head_;
    }
  }

  auto node = std::make_unique<Node>(
      std::move(internal_key), Entry{sequence, std::move(value), deleted}, new_level);
  // Finish every successor link before making the node reachable at any level.
  for (int i = 0; i < new_level; ++i) {
    const size_t level = static_cast<size_t>(i);
    Node* source = replacing ? candidate : prev[level];
    node->next[level].store(source->next[level].load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
  }
  if (replacing) retired_nodes_.emplace_back(candidate);
  Node* published = node.release();
  for (int i = 0; i < new_level; ++i) {
    prev[static_cast<size_t>(i)]->next[static_cast<size_t>(i)].store(
        published, std::memory_order_release);
  }
  if (new_level > old_level) level_.store(new_level, std::memory_order_relaxed);
  if (!replacing) {
    size_.fetch_add(1, std::memory_order_relaxed);
  }
}

std::optional<Entry> SkipList::Get(const std::string& key,
                                   SequenceNumber read_sequence) const {
  Node* candidate = FindGreaterOrEqual(EncodeInternalKey(key, read_sequence), nullptr);
  if (candidate == nullptr) {
    return std::nullopt;
  }
  // Nodes contain only keys produced by EncodeInternalKey(); the seek already
  // selects a sequence no newer than read_sequence for this user key.
  const std::string_view candidate_key(candidate->internal_key.data(),
      candidate->internal_key.size() - 1 - sizeof(SequenceNumber));
  if (candidate_key != key) {
    return std::nullopt;
  }
  return candidate->entry;
}

std::vector<VersionedEntry> SkipList::Entries() const {
  std::vector<VersionedEntry> entries;
  entries.reserve(size_.load(std::memory_order_relaxed));
  Node* node = head_.next[0].load(std::memory_order_acquire);
  while (node != nullptr) {
    entries.push_back(VersionedEntry{
        node->internal_key.substr(0, node->internal_key.size() - 1 - sizeof(SequenceNumber)),
        node->entry.sequence, node->entry.value, node->entry.deleted});
    node = node->next[0].load(std::memory_order_acquire);
  }
  return entries;
}

size_t SkipList::Size() const {
  return size_.load(std::memory_order_relaxed);
}

}  // namespace kv
