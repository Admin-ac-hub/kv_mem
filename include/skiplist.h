#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace kv {

// Writes must be externally serialized. Readers may run concurrently with the
// writer; destruction requires all accesses to have finished. Published node
// keys/values never change, and nodes are reclaimed only with the whole table.
class SkipList {
 public:
  SkipList();
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Put(std::string key, SequenceNumber sequence, std::string value);
  void Delete(std::string key, SequenceNumber sequence);
  std::optional<Entry> Get(const std::string& key, SequenceNumber read_sequence) const;
  std::vector<VersionedEntry> Entries() const;
  size_t Size() const;

 private:
  void Clear();

  struct Node {
    const std::string internal_key;
    const Entry entry;
    std::vector<std::atomic<Node*>> next;

    Node(std::string internal_key_in, Entry entry_in, int level)
        : internal_key(std::move(internal_key_in)),
          entry(std::move(entry_in)),
          next(static_cast<size_t>(level)) {
      for (auto& link : next) link.store(nullptr, std::memory_order_relaxed);
    }
  };

  static constexpr int kMaxLevel = 12;
  static constexpr double kBranching = 0.5;

  int RandomLevel();
  void Insert(std::string key,
              SequenceNumber sequence,
              std::string value,
              bool deleted);
  Node* FindGreaterOrEqual(const std::string& internal_key,
                           std::vector<Node*>* prev) const;

  Node head_;
  // Height is only a search hint; observing an older height is safe.
  std::atomic<int> level_;
  std::atomic<size_t> size_{0};
  std::mt19937 rng_;
  // Replacing an identical internal key must keep the old payload alive for
  // readers already holding that node. DB sequences normally avoid this path.
  std::vector<std::unique_ptr<Node>> retired_nodes_;
};

}  // namespace kv
