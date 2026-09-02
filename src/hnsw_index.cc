#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <utility>

namespace kv {

namespace {

constexpr int kMaximumRandomLevel = 63;

}  // namespace

bool HNSWIndex::CandidateIsBetter(const Candidate& lhs,
                                  const Candidate& rhs) {
  if (lhs.distance != rhs.distance) {
    return lhs.distance < rhs.distance;
  }
  return lhs.id < rhs.id;
}

bool HNSWIndex::CandidateMinHeap::operator()(const Candidate& lhs,
                                             const Candidate& rhs) const {
  return CandidateIsBetter(rhs, lhs);
}

bool HNSWIndex::CandidateMaxHeap::operator()(const Candidate& lhs,
                                             const Candidate& rhs) const {
  return CandidateIsBetter(lhs, rhs);
}

HNSWIndex::HNSWIndex(HNSWOptions options)
    : options_(std::move(options)), random_(options_.random_seed) {
  if (options_.max_neighbors > 1) {
    level_multiplier_ = 1.0 / std::log(
        static_cast<double>(options_.max_neighbors));
  }
}

Status HNSWIndex::ValidateOptions() const {
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

Status HNSWIndex::ValidateVector(const std::vector<float>& vector) const {
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

int HNSWIndex::SelectLevel() {
  std::uniform_real_distribution<double> distribution(
      std::numeric_limits<double>::min(), 1.0);
  const double sample = distribution(random_);
  const double level = -std::log(sample) * level_multiplier_;
  return std::min(static_cast<int>(level), kMaximumRandomLevel);
}

float HNSWIndex::Distance(const std::vector<float>& lhs,
                          const std::vector<float>& rhs) const {
  double distance = 0.0;
  switch (options_.metric) {
    case VectorDistanceMetric::kL2: {
      double squared_l2 = 0.0;
      for (size_t i = 0; i < lhs.size(); ++i) {
        const double difference =
            static_cast<double>(lhs[i]) - static_cast<double>(rhs[i]);
        squared_l2 += difference * difference;
      }
      distance = std::sqrt(squared_l2);
      break;
    }
    case VectorDistanceMetric::kInnerProduct: {
      double dot = 0.0;
      for (size_t i = 0; i < lhs.size(); ++i) {
        dot += static_cast<double>(lhs[i]) * static_cast<double>(rhs[i]);
      }
      distance = -dot;
      break;
    }
    case VectorDistanceMetric::kCosine: {
      double dot = 0.0;
      double lhs_norm = 0.0;
      double rhs_norm = 0.0;
      for (size_t i = 0; i < lhs.size(); ++i) {
        const double left = lhs[i];
        const double right = rhs[i];
        dot += left * right;
        lhs_norm += left * left;
        rhs_norm += right * right;
      }
      distance = 1.0 - std::clamp(
          dot / std::sqrt(lhs_norm * rhs_norm), -1.0, 1.0);
      break;
    }
  }

  const double maximum = std::numeric_limits<float>::max();
  return static_cast<float>(std::clamp(distance, -maximum, maximum));
}

std::vector<HNSWIndex::Candidate> HNSWIndex::SearchLayer(
    const std::vector<float>& query,
    const std::vector<NodeId>& entry_points,
    size_t ef,
    int level,
    const SequenceNumber* read_sequence) const {
  std::priority_queue<Candidate, std::vector<Candidate>, CandidateMinHeap>
      candidates;
  std::priority_queue<Candidate, std::vector<Candidate>, CandidateMaxHeap>
      nearest;
  std::vector<bool> visited(nodes_.size(), false);

  for (NodeId entry : entry_points) {
    if (entry >= nodes_.size() || visited[entry] ||
        level >= static_cast<int>(nodes_[entry].neighbors.size())) {
      continue;
    }
    visited[entry] = true;
    Candidate candidate{Distance(query, nodes_[entry].vector), entry};
    candidates.push(candidate);
    if (read_sequence == nullptr || IsVisible(entry, *read_sequence)) {
      nearest.push(candidate);
      if (nearest.size() > ef) {
        nearest.pop();
      }
    }
  }

  while (!candidates.empty()) {
    const Candidate current = candidates.top();
    if (nearest.size() >= ef &&
        current.distance > nearest.top().distance) {
      break;
    }
    candidates.pop();

    for (NodeId neighbor : nodes_[current.id].neighbors[level]) {
      if (neighbor >= nodes_.size() || visited[neighbor]) {
        continue;
      }
      visited[neighbor] = true;
      Candidate candidate{Distance(query, nodes_[neighbor].vector), neighbor};
      if (nearest.size() < ef ||
          CandidateIsBetter(candidate, nearest.top())) {
        candidates.push(candidate);
        if (read_sequence == nullptr || IsVisible(neighbor, *read_sequence)) {
          nearest.push(candidate);
          if (nearest.size() > ef) {
            nearest.pop();
          }
        }
      }
    }
  }

  std::vector<Candidate> result;
  result.reserve(nearest.size());
  while (!nearest.empty()) {
    result.push_back(nearest.top());
    nearest.pop();
  }
  std::sort(result.begin(), result.end(), CandidateIsBetter);
  return result;
}

bool HNSWIndex::IsVisible(NodeId node_id,
                          SequenceNumber read_sequence) const {
  if (node_id >= nodes_.size()) {
    return false;
  }
  const Node& node = nodes_[node_id];
  const auto versions_it = versions_by_key_.find(node.key);
  if (versions_it == versions_by_key_.end()) {
    return false;
  }
  const auto& versions = versions_it->second;
  auto visible = versions.upper_bound(read_sequence);
  if (visible == versions.begin()) {
    return false;
  }
  --visible;
  return !visible->second.deleted && visible->second.node_id == node_id;
}

std::vector<HNSWIndex::NodeId> HNSWIndex::SelectNeighbors(
    const std::vector<Candidate>& candidates,
    size_t limit) const {
  std::vector<NodeId> selected;
  std::vector<NodeId> pruned;
  selected.reserve(std::min(limit, candidates.size()));

  for (const Candidate& candidate : candidates) {
    bool diverse = true;
    for (NodeId neighbor : selected) {
      if (Distance(nodes_[candidate.id].vector,
                   nodes_[neighbor].vector) < candidate.distance) {
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

  for (NodeId candidate : pruned) {
    selected.push_back(candidate);
    if (selected.size() == limit) {
      break;
    }
  }
  return selected;
}

void HNSWIndex::PruneNeighbors(NodeId node_id, int level) {
  auto& neighbors = nodes_[node_id].neighbors[level];
  if (neighbors.size() <= options_.max_neighbors) {
    return;
  }

  std::vector<Candidate> candidates;
  candidates.reserve(neighbors.size());
  for (NodeId neighbor : neighbors) {
    candidates.push_back(
        {Distance(nodes_[node_id].vector, nodes_[neighbor].vector), neighbor});
  }
  std::sort(candidates.begin(), candidates.end(), CandidateIsBetter);
  neighbors = SelectNeighbors(candidates, options_.max_neighbors);
}

void HNSWIndex::Connect(NodeId node_id,
                        const std::vector<NodeId>& neighbors,
                        int level) {
  nodes_[node_id].neighbors[level] = neighbors;
  for (NodeId neighbor : neighbors) {
    auto& reverse = nodes_[neighbor].neighbors[level];
    if (std::find(reverse.begin(), reverse.end(), node_id) == reverse.end()) {
      reverse.push_back(node_id);
      PruneNeighbors(neighbor, level);
    }
  }
}

Status HNSWIndex::Insert(std::string key,
                         const std::vector<float>& vector,
                         std::string metadata) {
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

  std::unique_lock<std::shared_mutex> lock(mutex_);
  return InsertVersionLocked(std::move(key), 0, vector, std::move(metadata),
                             false);
}

Status HNSWIndex::InsertVersion(std::string key,
                                SequenceNumber sequence,
                                const std::vector<float>& vector,
                                std::string metadata) {
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

  std::unique_lock<std::shared_mutex> lock(mutex_);
  return InsertVersionLocked(std::move(key), sequence, vector,
                             std::move(metadata), true);
}

Status HNSWIndex::InsertVersionLocked(std::string key,
                                      SequenceNumber sequence,
                                      const std::vector<float>& vector,
                                      std::string metadata,
                                      bool allow_existing_key) {
  auto versions_it = versions_by_key_.find(key);
  if (!allow_existing_key && versions_it != versions_by_key_.end()) {
    return Status::InvalidArgument("HNSW key already exists");
  }
  if (versions_it != versions_by_key_.end() &&
      versions_it->second.find(sequence) != versions_it->second.end()) {
    return Status::InvalidArgument("HNSW key version already exists");
  }

  const int level = SelectLevel();
  const NodeId node_id = nodes_.size();
  Node node;
  node.id = node_id;
  node.key = key;
  node.sequence = sequence;
  node.vector = vector;
  node.metadata = std::move(metadata);
  node.neighbors.resize(static_cast<size_t>(level) + 1);
  nodes_.push_back(std::move(node));
  versions_by_key_[std::move(key)].emplace(
      sequence, VersionState{node_id, false});

  if (nodes_.size() == 1) {
    entry_point_ = node_id;
    max_level_ = level;
    return Status::OK();
  }

  std::vector<NodeId> entry_points = {entry_point_};
  for (int current_level = max_level_; current_level > level;
       --current_level) {
    const auto nearest = SearchLayer(vector, entry_points, 1, current_level);
    entry_points.assign(1, nearest.front().id);
  }

  const int first_connected_level = std::min(level, max_level_);
  for (int current_level = first_connected_level; current_level >= 0;
       --current_level) {
    const auto candidates = SearchLayer(
        vector, entry_points,
        std::max(options_.ef_construction, options_.max_neighbors),
        current_level);
    const auto neighbors = SelectNeighbors(candidates, options_.max_neighbors);
    Connect(node_id, neighbors, current_level);

    entry_points.clear();
    entry_points.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
      entry_points.push_back(candidate.id);
    }
  }

  if (level > max_level_) {
    entry_point_ = node_id;
    max_level_ = level;
  }
  return Status::OK();
}

Status HNSWIndex::MarkDeleted(std::string key, SequenceNumber sequence) {
  Status status = ValidateOptions();
  if (!status.ok()) {
    return status;
  }
  if (key.empty()) {
    return Status::InvalidArgument("HNSW key cannot be empty");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto& versions = versions_by_key_[std::move(key)];
  if (versions.find(sequence) != versions.end()) {
    return Status::InvalidArgument("HNSW key version already exists");
  }
  versions.emplace(sequence, VersionState{0, true});
  return Status::OK();
}

Status HNSWIndex::Search(const std::vector<float>& query,
                         size_t top_k,
                         size_t ef_search,
                         std::vector<VectorResult>* results) const {
  return SearchAtSequence(query, top_k, ef_search,
                          std::numeric_limits<SequenceNumber>::max(), results);
}

Status HNSWIndex::SearchAtSequence(
    const std::vector<float>& query,
    size_t top_k,
    size_t ef_search,
    SequenceNumber read_sequence,
    std::vector<VectorResult>* results) const {
  if (results == nullptr) {
    return Status::InvalidArgument("HNSW search results cannot be null");
  }
  results->clear();
  Status status = ValidateOptions();
  if (!status.ok()) {
    return status;
  }
  status = ValidateVector(query);
  if (!status.ok()) {
    return status;
  }
  if (ef_search == 0) {
    return Status::InvalidArgument("HNSW ef_search must be greater than zero");
  }
  if (top_k == 0) {
    return Status::OK();
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (nodes_.empty()) {
    return Status::OK();
  }

  std::vector<NodeId> entry_points = {entry_point_};
  for (int level = max_level_; level > 0; --level) {
    const auto nearest = SearchLayer(query, entry_points, 1, level);
    entry_points.assign(1, nearest.front().id);
  }

  const auto nearest = SearchLayer(
      query, entry_points, std::max(top_k, ef_search), 0, &read_sequence);
  const size_t result_count = std::min(top_k, nearest.size());
  results->reserve(result_count);
  for (size_t i = 0; i < result_count; ++i) {
    const Node& node = nodes_[nearest[i].id];
    results->push_back({node.key, nearest[i].distance, node.metadata});
  }
  std::sort(results->begin(), results->end(),
            [](const VectorResult& lhs, const VectorResult& rhs) {
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              return lhs.key < rhs.key;
            });
  return Status::OK();
}

size_t HNSWIndex::Size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return nodes_.size();
}

HNSWStats HNSWIndex::Stats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  HNSWStats stats;
  stats.node_count = nodes_.size();
  stats.max_level = max_level_;
  if (max_level_ >= 0) {
    stats.nodes_per_level.resize(static_cast<size_t>(max_level_) + 1, 0);
  }
  for (const Node& node : nodes_) {
    for (size_t level = 0; level < node.neighbors.size(); ++level) {
      ++stats.nodes_per_level[level];
      stats.directed_edge_count += node.neighbors[level].size();
      stats.max_neighbor_count = std::max(
          stats.max_neighbor_count, node.neighbors[level].size());
    }
  }
  return stats;
}

}  // namespace kv
