#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "status.h"
#include "types.h"
#include "vector_value.h"

namespace kv {

struct HNSWOptions {
  size_t dimension = 0;
  // M: new nodes select M neighbors; reverse links may grow level 0 to 2*M.
  size_t max_neighbors = 16;
  size_t ef_construction = 200;
  VectorDistanceMetric metric = VectorDistanceMetric::kL2;
  std::uint64_t random_seed = 0x48534e57ULL;//"HNSW"的ASCII
};

struct HNSWStats {
  size_t node_count = 0;
  size_t directed_edge_count = 0;
  size_t max_neighbor_count = 0;
  int max_level = -1;
  std::vector<size_t> nodes_per_level;
  std::vector<size_t> max_neighbors_per_level;
};

// HNSW index. Insertions and file loads are serialized while searches may run
// concurrently. Version visibility is tracked by sequence and tombstones.
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
  // MemTables and in-flight searches hold shared pointers to this object.
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
  using NodeId = std::uint64_t;

  struct Node {
    NodeId id = 0;
    std::string key;
    SequenceNumber sequence = 0;
    // Vectors live in vector_data_ as one contiguous, cache-friendly array.
    size_t vector_offset = 0;
    std::string metadata;
    std::vector<std::vector<NodeId>> neighbors;
  };

  struct Candidate {
    float distance = 0.0f;
    NodeId id = 0;
  };

  struct VersionState {
    NodeId node_id = 0;
    bool deleted = false;
  };

  struct CandidateMinHeap {
    bool operator()(const Candidate& lhs, const Candidate& rhs) const;
  };

  struct CandidateMaxHeap {
    bool operator()(const Candidate& lhs, const Candidate& rhs) const;
  };

  Status ValidateOptions() const;
  Status ValidateVector(const std::vector<float>& vector) const;
  static bool CandidateIsBetter(const Candidate& lhs,
                                const Candidate& rhs);
  int SelectLevel();
  float Distance(const float* lhs, const float* rhs) const;
  const float* VectorData(NodeId node_id) const;
  bool VectorEquals(NodeId node_id, const std::vector<float>& vector) const;
  std::vector<Candidate> SearchLayer(
      const std::vector<float>& query,
      const std::vector<NodeId>& entry_points,
      size_t ef,
      int level,
      const SequenceNumber* read_sequence = nullptr) const;
  std::vector<NodeId> SelectNeighbors(
      const std::vector<Candidate>& candidates,
      size_t limit) const;
  void Connect(NodeId node_id,
               const std::vector<NodeId>& neighbors,
               int level);
  void PruneNeighbors(NodeId node_id, int level);
  size_t NeighborLimit(int level) const;
  bool IsVisible(NodeId node_id, SequenceNumber read_sequence) const;
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
  double level_multiplier_ = 0.0;
  int max_level_ = -1;
  NodeId entry_point_ = 0;
  std::vector<Node> nodes_;
  std::vector<float> vector_data_;
  std::unordered_map<std::string,
                     std::map<SequenceNumber, VersionState>> versions_by_key_;
  std::mt19937_64 random_;
  mutable std::shared_mutex mutex_;
};

}  // namespace kv
