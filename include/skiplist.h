#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace kv {

class SkipList {
 public:
  struct Entry {
    SequenceNumber sequence = 0;
    std::string value;
    bool deleted = false;
  };

  SkipList();
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Put(std::string key, SequenceNumber sequence, std::string value);
  void Delete(std::string key, SequenceNumber sequence);
  std::optional<Entry> Get(const std::string& key, SequenceNumber read_sequence) const;
  std::vector<VersionedEntry> Entries() const;
  size_t Size() const;
  void Clear();

 private:
  struct Node {
    std::string internal_key;
    Entry entry;
    std::vector<Node*> next;

    Node(std::string internal_key_in, Entry entry_in, int level)
        : internal_key(std::move(internal_key_in)),
          entry(std::move(entry_in)),
          next(static_cast<size_t>(level), nullptr) {}
  };

  static constexpr int kMaxLevel = 12;
  static constexpr double kBranching = 0.5;

  int RandomLevel();
  Node* FindGreaterOrEqual(const std::string& internal_key,
                           std::vector<Node*>* prev) const;

  Node head_;
  int level_;
  size_t size_ = 0;
  mutable std::mt19937 rng_;
};

}  // namespace kv
