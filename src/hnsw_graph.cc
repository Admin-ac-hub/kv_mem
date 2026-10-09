#include "hnsw_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace kv {

namespace {

constexpr int kMaximumRandomLevel = 63;

// Ties are broken by node id so that heap order is total and deterministic.
bool CandidateIsBetterImpl(const HNSWCandidate& lhs, const HNSWCandidate& rhs) {
  if (lhs.distance != rhs.distance) {
    return lhs.distance < rhs.distance;
  }
  return lhs.id < rhs.id;
}

struct CandidateMinHeap {
  bool operator()(const HNSWCandidate& lhs, const HNSWCandidate& rhs) const {
    return CandidateIsBetterImpl(rhs, lhs);
  }
};

struct CandidateMaxHeap {
  bool operator()(const HNSWCandidate& lhs, const HNSWCandidate& rhs) const {
    return CandidateIsBetterImpl(lhs, rhs);
  }
};

}  // namespace

#ifdef KV_HNSW_DISTANCE_COUNTER
namespace {
thread_local std::uint64_t g_distance_calls = 0;
}  // namespace

std::uint64_t HNSWDistanceCallCount() { return g_distance_calls; }
void HNSWResetDistanceCallCount() { g_distance_calls = 0; }
#else
std::uint64_t HNSWDistanceCallCount() { return 0; }
void HNSWResetDistanceCallCount() {}
#endif

bool HNSWGraph::CandidateIsBetter(const HNSWCandidate& lhs,
                                  const HNSWCandidate& rhs) {
  return CandidateIsBetterImpl(lhs, rhs);
}

HNSWGraph::HNSWGraph(HNSWGraphOptions options)
    : options_(std::move(options)), random_(options_.random_seed) {
  if (options_.max_neighbors > 1) {
    level_multiplier_ = 1.0 / std::log(
        static_cast<double>(options_.max_neighbors));
  }
}

Status HNSWGraph::ValidateOptions() const {
  if (options_.dimension == 0) {
    return Status::InvalidArgument("HNSW dimension must be greater than zero");
  }
  if (options_.max_neighbors < 2) {
    return Status::InvalidArgument(
        "HNSW max_neighbors must be at least two");
  }
  if (options_.ef_construction == 0) {
    return Status::InvalidArgument(
        "HNSW ef_construction must be greater than zero");
  }
  if (options_.dimension > std::numeric_limits<std::uint32_t>::max() ||
      options_.max_neighbors > std::numeric_limits<std::uint32_t>::max() / 2 ||
      options_.ef_construction > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("HNSW options exceed file format limits");
  }
  switch (options_.metric) {
    case VectorDistanceMetric::kL2:
    case VectorDistanceMetric::kInnerProduct:
    case VectorDistanceMetric::kCosine:
      break;
    default:
      return Status::InvalidArgument("unknown HNSW distance metric");
  }
  return Status::OK();
}

Status HNSWGraph::ValidateVector(const std::vector<float>& vector) const {
  if (vector.size() != options_.dimension) {
    return Status::InvalidArgument(
        "vector dimension does not match HNSW dimension");
  }
  bool has_nonzero_value = false;
  for (float value : vector) {
    if (!std::isfinite(value)) {
      return Status::InvalidArgument("vector values must be finite");
    }
    has_nonzero_value = has_nonzero_value || value != 0.0f;
  }
  if (options_.metric == VectorDistanceMetric::kCosine &&
      !has_nonzero_value) {
    return Status::InvalidArgument(
        "cosine distance is undefined for a zero vector");
  }
  return Status::OK();
}

int HNSWGraph::SelectLevel() {
  std::uniform_real_distribution<double> distribution(
      std::numeric_limits<double>::min(), 1.0);
  const double sample = distribution(random_);
  const double level = -std::log(sample) * level_multiplier_;
  return std::min(static_cast<int>(level), kMaximumRandomLevel);
}

float HNSWGraph::Distance(const float* lhs, const float* rhs) const {
#ifdef KV_HNSW_DISTANCE_COUNTER
  ++g_distance_calls;
#endif
  return ComputeVectorDistanceUnchecked(lhs, rhs, options_.dimension,
                                        options_.metric);
}

const float* HNSWGraph::VectorData(HNSWNodeId node_id) const {
  return vector_data_.data() + nodes_[node_id].vector_offset;
}

const float* HNSWGraph::VectorOf(HNSWNodeId node_id) const {
  return VectorData(node_id);
}

bool HNSWGraph::VectorEquals(HNSWNodeId node_id,
                             const std::vector<float>& vector) const {
  const float* stored = VectorData(node_id);
  return std::equal(vector.begin(), vector.end(), stored);
}

std::vector<HNSWCandidate> HNSWGraph::SearchLayer(
    const std::vector<float>& query,
    const std::vector<HNSWNodeId>& entry_points, size_t ef, int level,
    const HNSWNodeVisibility* visibility) const {
  if (ef == 0 || level < 0) {
    return {};
  }
  std::priority_queue<HNSWCandidate, std::vector<HNSWCandidate>,
                      CandidateMinHeap>
      candidates;
  std::priority_queue<HNSWCandidate, std::vector<HNSWCandidate>,
                      CandidateMaxHeap>
      nearest;
  // Keep the marker array per thread; each layer needs a fresh generation but
  // does not need to allocate or clear O(node_count) bits on every query. The
  // array is shared across graph instances in the same thread, so it grows to
  // the largest graph seen rather than to the sum of all graphs.
  thread_local std::vector<std::uint32_t> visit_marks;
  thread_local std::uint32_t visit_generation = 0;
  if (visit_marks.size() < nodes_.size()) {
    visit_marks.resize(nodes_.size(), 0);
  }
  if (++visit_generation == 0) {
    std::fill(visit_marks.begin(), visit_marks.end(), 0);
    visit_generation = 1;
  }
  const auto visited = [&](HNSWNodeId node_id) {
    return visit_marks[node_id] == visit_generation;
  };
  const auto mark_visited = [&](HNSWNodeId node_id) {
    visit_marks[node_id] = visit_generation;
  };

  for (HNSWNodeId entry : entry_points) {
    if (entry >= nodes_.size() || visited(entry) ||
        level >= static_cast<int>(nodes_[entry].neighbors.size())) {
      continue;
    }
    mark_visited(entry);
    HNSWCandidate candidate{Distance(query.data(), VectorData(entry)), entry};
    candidates.push(candidate);
    if (visibility == nullptr || visibility->IsVisible(entry, nodes_[entry].key)) {
      nearest.push(candidate);
      if (nearest.size() > ef) {
        nearest.pop();
      }
    }
  }

  while (!candidates.empty()) {
    const HNSWCandidate current = candidates.top();
    if (nearest.size() >= ef &&
        current.distance > nearest.top().distance) {
      break;
    }
    candidates.pop();

    for (HNSWNodeId neighbor : nodes_[current.id].neighbors[level]) {
      if (neighbor >= nodes_.size() || visited(neighbor) ||
          level >= static_cast<int>(nodes_[neighbor].neighbors.size())) {
        continue;
      }
      mark_visited(neighbor);
      HNSWCandidate candidate{Distance(query.data(), VectorData(neighbor)),
                              neighbor};
      if (nearest.size() < ef ||
          CandidateIsBetter(candidate, nearest.top())) {
        candidates.push(candidate);
        if (visibility == nullptr ||
            visibility->IsVisible(neighbor, nodes_[neighbor].key)) {
          nearest.push(candidate);
          if (nearest.size() > ef) {
            nearest.pop();
          }
        }
      }
    }
  }

  std::vector<HNSWCandidate> result;
  result.reserve(nearest.size());
  while (!nearest.empty()) {
    result.push_back(nearest.top());
    nearest.pop();
  }
  std::sort(result.begin(), result.end(), CandidateIsBetter);
  return result;
}

std::vector<HNSWNodeId> HNSWGraph::SelectNeighbors(
    const std::vector<HNSWCandidate>& candidates, size_t limit) const {
  std::vector<HNSWNodeId> selected;
  std::vector<HNSWNodeId> pruned;
  selected.reserve(std::min(limit, candidates.size()));

  for (const HNSWCandidate& candidate : candidates) {
    bool diverse = true;
    for (HNSWNodeId neighbor : selected) {
      if (Distance(VectorData(candidate.id), VectorData(neighbor)) <
          candidate.distance) {
        diverse = false;
        break;
      }
    }
    if (diverse) {
      selected.push_back(candidate.id);
      if (selected.size() == limit) {
        return selected;
      }
    } else {
      pruned.push_back(candidate.id);
    }
  }

  for (HNSWNodeId candidate : pruned) {
    selected.push_back(candidate);
    if (selected.size() == limit) {
      break;
    }
  }
  return selected;
}

size_t HNSWGraph::NeighborLimit(int level) const {
  return level == 0 ? 2 * options_.max_neighbors : options_.max_neighbors;
}

void HNSWGraph::PruneNeighbors(HNSWNodeId node_id, int level) {
  auto& neighbors = nodes_[node_id].neighbors[level];
  const size_t limit = NeighborLimit(level);
  if (neighbors.size() <= limit) {
    return;
  }

  std::vector<HNSWCandidate> candidates;
  candidates.reserve(neighbors.size());
  for (HNSWNodeId neighbor : neighbors) {
    candidates.push_back(
        {Distance(VectorData(node_id), VectorData(neighbor)), neighbor});
  }
  std::sort(candidates.begin(), candidates.end(), CandidateIsBetter);
  neighbors = SelectNeighbors(candidates, limit);
}

void HNSWGraph::Connect(HNSWNodeId node_id,
                        const std::vector<HNSWNodeId>& neighbors, int level) {
  nodes_[node_id].neighbors[level] = neighbors;
  for (HNSWNodeId neighbor : neighbors) {
    auto& reverse = nodes_[neighbor].neighbors[level];
    if (std::find(reverse.begin(), reverse.end(), node_id) == reverse.end()) {
      reverse.push_back(node_id);
      PruneNeighbors(neighbor, level);
    }
  }
}

Status HNSWGraph::AddNode(const std::vector<float>& vector, std::string key,
                          std::string metadata, HNSWNodeId* node_id) {
  if (node_id == nullptr) {
    return Status::InvalidArgument("HNSW node id output cannot be null");
  }
  Status status = ValidateOptions();
  if (!status.ok()) {
    return status;
  }
  if (key.empty()) {
    return Status::InvalidArgument("HNSW key cannot be empty");
  }
  status = ValidateVector(vector);
  if (!status.ok()) {
    return status;
  }

  const int level = SelectLevel();
  const HNSWNodeId id = nodes_.size();
  Node node;
  node.id = id;
  node.key = std::move(key);
  node.vector_offset = vector_data_.size();
  node.metadata = std::move(metadata);
  node.neighbors.resize(static_cast<size_t>(level) + 1);
  // Select links before publishing the node, so an invalid graph cannot leave
  // a partially inserted version behind when traversal fails.
  std::vector<HNSWNodeId> entry_points = {entry_point_};
  for (int current_level = max_level_; current_level > level;
       --current_level) {
    const auto nearest = SearchLayer(vector, entry_points, 1, current_level);
    if (nearest.empty()) {
      return Status::Corruption("HNSW insertion has no valid layer entry");
    }
    entry_points.assign(1, nearest.front().id);
  }

  const int first_connected_level = std::min(level, max_level_);
  for (int current_level = first_connected_level; current_level >= 0;
       --current_level) {
    const auto candidates = SearchLayer(
        vector, entry_points,
        std::max(options_.ef_construction, options_.max_neighbors),
        current_level);
    if (candidates.empty()) {
      return Status::Corruption("HNSW insertion has no valid layer entry");
    }
    node.neighbors[current_level] =
        SelectNeighbors(candidates, options_.max_neighbors);

    entry_points.clear();
    entry_points.reserve(candidates.size());
    for (const HNSWCandidate& candidate : candidates) {
      entry_points.push_back(candidate.id);
    }
  }

  vector_data_.insert(vector_data_.end(), vector.begin(), vector.end());
  nodes_.push_back(std::move(node));
  for (int current_level = first_connected_level; current_level >= 0;
       --current_level) {
    const auto neighbors = nodes_[id].neighbors[current_level];
    Connect(id, neighbors, current_level);
  }

  if (level > max_level_) {
    entry_point_ = id;
    max_level_ = level;
  }
  *node_id = id;
  return Status::OK();
}

Status HNSWGraph::Search(const std::vector<float>& query, size_t top_k,
                         size_t ef, const HNSWNodeVisibility* visibility,
                         std::vector<HNSWCandidate>* results) const {
  if (results == nullptr) {
    return Status::InvalidArgument("HNSW search results cannot be null");
  }
  results->clear();
  if (nodes_.empty() || top_k == 0) {
    return Status::OK();
  }

  std::vector<HNSWNodeId> entry_points = {entry_point_};
  for (int level = max_level_; level > 0; --level) {
    const auto nearest = SearchLayer(query, entry_points, 1, level);
    if (nearest.empty()) {
      return Status::Corruption("HNSW search has no valid layer entry");
    }
    entry_points.assign(1, nearest.front().id);
  }

  const auto nearest = SearchLayer(query, entry_points, std::max(top_k, ef), 0,
                                   visibility);
  const size_t result_count = std::min(top_k, nearest.size());
  results->reserve(result_count);
  for (size_t i = 0; i < result_count; ++i) {
    results->push_back(nearest[i]);
  }
  return Status::OK();
}

Status HNSWGraph::Replace(std::vector<HNSWGraphNode> nodes,
                          std::vector<float> vector_data,
                          HNSWNodeId entry_point, int max_level) {
  if (options_.dimension == 0) {
    return Status::InvalidArgument("HNSW dimension must be greater than zero");
  }
  if (nodes.size() > std::numeric_limits<size_t>::max() / options_.dimension) {
    return Status::Corruption("HNSW index vector data is too large");
  }
  if (vector_data.size() != nodes.size() * options_.dimension) {
    return Status::Corruption("HNSW index vector data size mismatch");
  }
  if (nodes.empty()) {
    if (max_level != -1) {
      return Status::Corruption("invalid HNSW index graph header");
    }
  } else if (max_level < 0 || entry_point >= nodes.size()) {
    return Status::Corruption("invalid HNSW index graph header");
  }

  std::vector<Node> replaced;
  replaced.reserve(nodes.size());
  for (size_t index = 0; index < nodes.size(); ++index) {
    Node node;
    node.id = index;
    node.key = std::move(nodes[index].key);
    node.metadata = std::move(nodes[index].metadata);
    node.vector_offset = index * options_.dimension;
    node.neighbors = std::move(nodes[index].neighbors);
    replaced.push_back(std::move(node));
  }

  nodes_ = std::move(replaced);
  vector_data_ = std::move(vector_data);
  max_level_ = nodes_.empty() ? -1 : max_level;
  entry_point_ = nodes_.empty() ? 0 : entry_point;
  return Status::OK();
}

void HNSWGraph::Clear() {
  nodes_.clear();
  vector_data_.clear();
  max_level_ = -1;
  entry_point_ = 0;
}

size_t HNSWGraph::LevelCount(HNSWNodeId node_id) const {
  return nodes_[node_id].neighbors.size();
}

const std::string& HNSWGraph::KeyOf(HNSWNodeId node_id) const {
  return nodes_[node_id].key;
}

const std::string& HNSWGraph::MetadataOf(HNSWNodeId node_id) const {
  return nodes_[node_id].metadata;
}

const std::vector<HNSWNodeId>& HNSWGraph::NeighborsOf(HNSWNodeId node_id,
                                                      int level) const {
  return nodes_[node_id].neighbors[static_cast<size_t>(level)];
}

HNSWGraphStats HNSWGraph::Stats() const {
  HNSWGraphStats stats;
  stats.node_count = nodes_.size();
  stats.max_level = max_level_;
  if (max_level_ >= 0) {
    stats.nodes_per_level.resize(static_cast<size_t>(max_level_) + 1, 0);
    stats.max_neighbors_per_level.resize(static_cast<size_t>(max_level_) + 1, 0);
  }
  for (const Node& node : nodes_) {
    for (size_t level = 0; level < node.neighbors.size(); ++level) {
      ++stats.nodes_per_level[level];
      stats.max_neighbors_per_level[level] = std::max(
          stats.max_neighbors_per_level[level], node.neighbors[level].size());
      stats.directed_edge_count += node.neighbors[level].size();
      stats.max_neighbor_count = std::max(
          stats.max_neighbor_count, node.neighbors[level].size());
    }
  }
  return stats;
}

}  // namespace kv
