#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "status.h"
#include "vector_value.h"

namespace kv {

using HNSWNodeId = std::uint64_t;

struct HNSWGraphOptions {
  size_t dimension = 0;
  // M: new nodes select M neighbors; reverse links may grow level 0 to 2*M.
  size_t max_neighbors = 16;
  size_t ef_construction = 200;
  VectorDistanceMetric metric = VectorDistanceMetric::kL2;
  std::uint64_t random_seed = 0x48534e57ULL;  // "HNSW" 的 ASCII
};

struct HNSWGraphStats {
  size_t node_count = 0;
  size_t directed_edge_count = 0;
  size_t max_neighbor_count = 0;
  int max_level = -1;
  std::vector<size_t> nodes_per_level;
  std::vector<size_t> max_neighbors_per_level;
};

struct HNSWCandidate {
  float distance = 0.0f;
  HNSWNodeId id = 0;
};

// Visibility is the only MVCC concept the graph is allowed to see, and it is
// supplied from outside. The graph never learns what makes a node invisible; it
// only asks before admitting a node to the result set. Traversal is unaffected:
// an invisible node still acts as a navigation stepping stone, which is exactly
// what the previous single-class implementation did.
class HNSWNodeVisibility {
 public:
  virtual ~HNSWNodeVisibility() = default;
  virtual bool IsVisible(HNSWNodeId node_id, const std::string& key) const = 0;
};

// A node as accepted by Replace(). Vector offsets are implied by node order and
// the configured dimension, matching the dense layout the file format uses.
struct HNSWGraphNode {
  std::string key;
  std::string metadata;
  std::vector<std::vector<HNSWNodeId>> neighbors;
};

// Pure graph structure: no sequence numbers, no tombstones, no files, no locks.
// One writer at a time is assumed; serializing writers is the caller's job.
//
// This is the layer that tools/hnsw_recall_probe.cpp and
// tools/hnsw_partition_probe.cpp drive, so it is testable with zero I/O and
// zero threads.
class HNSWGraph {
 public:
  explicit HNSWGraph(HNSWGraphOptions options);

  HNSWGraph(const HNSWGraph&) = delete;
  HNSWGraph& operator=(const HNSWGraph&) = delete;
  HNSWGraph(HNSWGraph&&) = default;
  HNSWGraph& operator=(HNSWGraph&&) = default;

  Status ValidateOptions() const;
  Status ValidateVector(const std::vector<float>& vector) const;

  // Appends a node and links it into the graph. On failure the graph is left
  // unchanged: nothing is published before every link has been selected.
  Status AddNode(const std::vector<float>& vector, std::string key,
                 std::string metadata, HNSWNodeId* node_id);

  // Top-k search starting from the graph's own entry point. `visibility` may be
  // null, in which case every node is admissible.
  Status Search(const std::vector<float>& query, size_t top_k, size_t ef,
                const HNSWNodeVisibility* visibility,
                std::vector<HNSWCandidate>* results) const;

  std::vector<HNSWCandidate> SearchLayer(
      const std::vector<float>& query,
      const std::vector<HNSWNodeId>& entry_points, size_t ef, int level,
      const HNSWNodeVisibility* visibility = nullptr) const;

  // Wholesale replacement, used by index loading. The caller parses and
  // validates the whole file first, so a corrupt file cannot partially replace
  // a live graph.
  Status Replace(std::vector<HNSWGraphNode> nodes,
                 std::vector<float> vector_data, HNSWNodeId entry_point,
                 int max_level);

  void Clear();

  size_t Size() const { return nodes_.size(); }
  int MaxLevel() const { return max_level_; }
  HNSWNodeId EntryPoint() const { return entry_point_; }
  // Degree cap at a given level: 2*M at level 0, M above it. Public because the
  // on-disk format is validated against it.
  size_t NeighborLimit(int level) const;
  size_t LevelCount(HNSWNodeId node_id) const;
  const std::string& KeyOf(HNSWNodeId node_id) const;
  const std::string& MetadataOf(HNSWNodeId node_id) const;
  const float* VectorOf(HNSWNodeId node_id) const;
  bool VectorEquals(HNSWNodeId node_id, const std::vector<float>& vector) const;
  const std::vector<HNSWNodeId>& NeighborsOf(HNSWNodeId node_id,
                                             int level) const;
  HNSWGraphStats Stats() const;

 private:
  struct Node {
    HNSWNodeId id = 0;
    std::string key;
    // Vectors live in vector_data_ as one contiguous, cache-friendly array.
    size_t vector_offset = 0;
    std::string metadata;
    std::vector<std::vector<HNSWNodeId>> neighbors;
  };

  static bool CandidateIsBetter(const HNSWCandidate& lhs,
                                const HNSWCandidate& rhs);
  int SelectLevel();
  float Distance(const float* lhs, const float* rhs) const;
  const float* VectorData(HNSWNodeId node_id) const;
  std::vector<HNSWNodeId> SelectNeighbors(
      const std::vector<HNSWCandidate>& candidates, size_t limit) const;
  void Connect(HNSWNodeId node_id, const std::vector<HNSWNodeId>& neighbors,
               int level);
  void PruneNeighbors(HNSWNodeId node_id, int level);

  HNSWGraphOptions options_;
  double level_multiplier_ = 0.0;
  int max_level_ = -1;
  HNSWNodeId entry_point_ = 0;
  std::vector<Node> nodes_;
  std::vector<float> vector_data_;
  std::mt19937_64 random_;
};

// Optional diagnostics. When KV_HNSW_DISTANCE_COUNTER is defined at compile
// time, HNSWGraph::Distance() increments a thread-local counter. Default builds
// contain no counter and no extra branch; both functions then return 0 / do
// nothing. Used to replace the modelled "distance calls per insert" figure in
// docs/VECTOR_INDEX_ARCHITECTURE.md section 1.3 with a measured one.
std::uint64_t HNSWDistanceCallCount();
void HNSWResetDistanceCallCount();

}  // namespace kv
