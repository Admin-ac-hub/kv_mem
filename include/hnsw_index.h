#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hnsw_graph.h"
#include "status.h"
#include "types.h"
#include "vector_value.h"

namespace kv {

// The graph owns the parameters and statistics; the index re-exports them for
// extension callers and standalone algorithm tools.
using HNSWOptions = HNSWGraphOptions;
using HNSWStats = HNSWGraphStats;

// HNSW index. Insertions and file loads are serialized while searches may run
// concurrently. Version visibility is tracked by sequence and tombstones.
//
// Stage 1 of docs/VECTOR_INDEX_ARCHITECTURE.md split the pure graph structure
// out into HNSWGraph. What remains here is exactly what the graph must not know
// about: sequence numbers, tombstones, the file format, and the mutex.
class HNSWIndex {
 public:
  explicit HNSWIndex(HNSWOptions options);

  HNSWIndex(const HNSWIndex&) = delete;
  HNSWIndex& operator=(const HNSWIndex&) = delete;

  Status Insert(std::string key,
                const std::vector<float>& vector,
                std::string metadata = {});
  Status InsertVersion(std::string key,
                       SequenceNumber sequence,
                       const std::vector<float>& vector,
                       std::string metadata = {});
  Status MarkDeleted(std::string key, SequenceNumber sequence);
  Status Save(const std::filesystem::path& path) const;
  Status Load(const std::filesystem::path& path);
  Status MergeFrom(const std::filesystem::path& path);
  // Atomically replace the graph state while keeping the index object alive.
  Status ReplaceWith(HNSWIndex&& other);
  Status Search(const std::vector<float>& query,
                size_t top_k,
                size_t ef_search,
                std::vector<VectorResult>* results) const;
  Status SearchAtSequence(const std::vector<float>& query,
                          size_t top_k,
                          size_t ef_search,
                          SequenceNumber read_sequence,
                          std::vector<VectorResult>* results) const;
  Status SearchBatch(const std::vector<std::vector<float>>& queries,
                     size_t top_k,
                     size_t ef_search,
                     std::vector<std::vector<VectorResult>>* results) const;
  Status SearchBatchAtSequence(
      const std::vector<std::vector<float>>& queries,
      size_t top_k,
      size_t ef_search,
      SequenceNumber read_sequence,
      std::vector<std::vector<VectorResult>>* results) const;

  size_t Size() const;
  HNSWStats Stats() const;

 private:
  using NodeId = HNSWNodeId;

  struct VersionState {
    NodeId node_id = 0;
    bool deleted = false;
  };

  // Per-call visibility predicate handed to HNSWGraph::Search. It is built on
  // the stack so that concurrent searches on the same index share no mutable
  // state -- the read sequence travels with the call, exactly as before.
  class SequenceVisibility final : public HNSWNodeVisibility {
   public:
    SequenceVisibility(const HNSWIndex* index, SequenceNumber read_sequence)
        : index_(index), read_sequence_(read_sequence) {}

    bool IsVisible(HNSWNodeId node_id, const std::string& key) const override;

   private:
    const HNSWIndex* index_;
    SequenceNumber read_sequence_;
  };

  Status ValidateOptions() const;
  Status ValidateVector(const std::vector<float>& vector) const;
  bool IsVisibleAt(NodeId node_id, const std::string& key,
                   SequenceNumber read_sequence) const;
  Status InsertVersionLocked(std::string key,
                             SequenceNumber sequence,
                             const std::vector<float>& vector,
                             std::string metadata,
                             bool allow_existing_key);
  Status SearchAtSequenceLocked(const std::vector<float>& query,
                                size_t top_k,
                                size_t ef_search,
                                SequenceNumber read_sequence,
                                std::vector<VectorResult>* results) const;

  HNSWOptions options_;
  HNSWGraph graph_;
  std::unordered_map<std::string,
                     std::map<SequenceNumber, VersionState>> versions_by_key_;
  mutable std::shared_mutex mutex_;
};

}  // namespace kv
