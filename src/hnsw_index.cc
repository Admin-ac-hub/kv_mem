#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <queue>
#include <string_view>
#include <utility>

#include "format.h"

namespace kv {

namespace {

constexpr int kMaximumRandomLevel = 63;
constexpr std::uint64_t kIndexMagic = 0x4b56484e535731ULL;  // KVHNSW1
// Version 2 permits 2*M neighbors at level 0. Version 1 remains readable.
constexpr std::uint32_t kIndexVersion = 2;
constexpr size_t kIndexHeaderSize = 56;
constexpr size_t kIndexFooterSize = 4;

void AppendFixed32(std::string* output, std::uint32_t value) {
  const size_t offset = output->size();
  output->resize(offset + sizeof(value));
  EncodeFixed32(value, output->data() + offset);
}

void AppendFixed64(std::string* output, std::uint64_t value) {
  const size_t offset = output->size();
  output->resize(offset + sizeof(value));
  EncodeFixed64(value, output->data() + offset);
}

bool FitsFixedString(const std::string& value) {
  return value.size() <= std::numeric_limits<std::uint32_t>::max();
}

Status ReadFixed32(std::string_view input,
                   size_t* offset,
                   std::uint32_t* value) {
  if (*offset > input.size() || input.size() - *offset < sizeof(*value)) {
    return Status::Corruption("truncated HNSW index");
  }
  *value = DecodeFixed32(input.data() + *offset);
  *offset += sizeof(*value);
  return Status::OK();
}

Status ReadFixed64(std::string_view input,
                   size_t* offset,
                   std::uint64_t* value) {
  if (*offset > input.size() || input.size() - *offset < sizeof(*value)) {
    return Status::Corruption("truncated HNSW index");
  }
  *value = DecodeFixed64(input.data() + *offset);
  *offset += sizeof(*value);
  return Status::OK();
}

Status ReadString(std::string_view input,
                  size_t* offset,
                  std::string* value) {
  std::uint32_t size = 0;
  Status status = ReadFixed32(input, offset, &size);
  if (!status.ok()) {
    return status;
  }
  if (*offset > input.size() || input.size() - *offset < size) {
    return Status::Corruption("truncated HNSW index string");
  }
  value->assign(input.data() + *offset, size);
  *offset += size;
  return Status::OK();
}

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

float HNSWIndex::Distance(const float* lhs, const float* rhs) const {
  return ComputeVectorDistanceUnchecked(lhs, rhs, options_.dimension,
                                        options_.metric);
}

const float* HNSWIndex::VectorData(NodeId node_id) const {
  return vector_data_.data() + nodes_[node_id].vector_offset;
}

bool HNSWIndex::VectorEquals(NodeId node_id,
                             const std::vector<float>& vector) const {
  const float* stored = VectorData(node_id);
  return std::equal(vector.begin(), vector.end(), stored);
}

std::vector<HNSWIndex::Candidate> HNSWIndex::SearchLayer(
    const std::vector<float>& query,
    const std::vector<NodeId>& entry_points,
    size_t ef,
    int level,
    const SequenceNumber* read_sequence) const {
  if (ef == 0 || level < 0) {
    return {};
  }
  std::priority_queue<Candidate, std::vector<Candidate>, CandidateMinHeap>
      candidates;
  std::priority_queue<Candidate, std::vector<Candidate>, CandidateMaxHeap>
      nearest;
  // Keep the marker array per thread; each layer needs a fresh generation but
  // does not need to allocate or clear O(node_count) bits on every query.
  thread_local std::vector<std::uint32_t> visit_marks;
  thread_local std::uint32_t visit_generation = 0;
  if (visit_marks.size() < nodes_.size()) {
    visit_marks.resize(nodes_.size(), 0);
  }
  if (++visit_generation == 0) {
    std::fill(visit_marks.begin(), visit_marks.end(), 0);
    visit_generation = 1;
  }
  const auto visited = [&](NodeId node_id) {
    return visit_marks[node_id] == visit_generation;
  };
  const auto mark_visited = [&](NodeId node_id) {
    visit_marks[node_id] = visit_generation;
  };

  for (NodeId entry : entry_points) {
    if (entry >= nodes_.size() || visited(entry) ||
        level >= static_cast<int>(nodes_[entry].neighbors.size())) {
      continue;
    }
    mark_visited(entry);
    Candidate candidate{Distance(query.data(), VectorData(entry)), entry};
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
      if (neighbor >= nodes_.size() || visited(neighbor) ||
          level >= static_cast<int>(nodes_[neighbor].neighbors.size())) {
        continue;
      }
      mark_visited(neighbor);
      Candidate candidate{Distance(query.data(), VectorData(neighbor)), neighbor};
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

  for (NodeId candidate : pruned) {
    selected.push_back(candidate);
    if (selected.size() == limit) {
      break;
    }
  }
  return selected;
}

size_t HNSWIndex::NeighborLimit(int level) const {
  return level == 0 ? 2 * options_.max_neighbors : options_.max_neighbors;
}

void HNSWIndex::PruneNeighbors(NodeId node_id, int level) {
  auto& neighbors = nodes_[node_id].neighbors[level];
  const size_t limit = NeighborLimit(level);
  if (neighbors.size() <= limit) {
    return;
  }

  std::vector<Candidate> candidates;
  candidates.reserve(neighbors.size());
  for (NodeId neighbor : neighbors) {
    candidates.push_back(
        {Distance(VectorData(node_id), VectorData(neighbor)), neighbor});
  }
  std::sort(candidates.begin(), candidates.end(), CandidateIsBetter);
  neighbors = SelectNeighbors(candidates, limit);
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
  if (versions_it != versions_by_key_.end()) {
    const auto existing = versions_it->second.find(sequence);
    if (existing != versions_it->second.end()) {
      if (existing->second.deleted) {
        return Status::InvalidArgument("HNSW key version conflicts with tombstone");
      }
      const Node& node = nodes_[existing->second.node_id];
      if (VectorEquals(node.id, vector) && node.metadata == metadata) {
        return Status::OK();
      }
      return Status::InvalidArgument("HNSW key version already exists");
    }
  }

  const int level = SelectLevel();
  const NodeId node_id = nodes_.size();
  Node node;
  node.id = node_id;
  node.key = key;
  node.sequence = sequence;
  node.vector_offset = vector_data_.size();
  node.metadata = std::move(metadata);
  node.neighbors.resize(static_cast<size_t>(level) + 1);
  // Select links before publishing the node, so an invalid graph cannot leave
  // a partially inserted version behind when traversal fails.
  std::vector<NodeId> entry_points = {entry_point_};
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
    for (const Candidate& candidate : candidates) {
      entry_points.push_back(candidate.id);
    }
  }

  vector_data_.insert(vector_data_.end(), vector.begin(), vector.end());
  nodes_.push_back(std::move(node));
  versions_by_key_[std::move(key)].emplace(
      sequence, VersionState{node_id, false});
  for (int current_level = first_connected_level; current_level >= 0;
       --current_level) {
    const auto neighbors = nodes_[node_id].neighbors[current_level];
    Connect(node_id, neighbors, current_level);
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
  auto& versions = versions_by_key_[key];
  const auto existing = versions.find(sequence);
  if (existing != versions.end()) {
    if (existing->second.deleted) {
      return Status::OK();
    }
    return Status::InvalidArgument("HNSW key version already exists");
  }
  versions.emplace(sequence, VersionState{0, true});
  return Status::OK();
}

Status HNSWIndex::Save(const std::filesystem::path& path) const {
  Status status = ValidateOptions();
  if (!status.ok()) {
    return status;
  }

  std::string body;
  std::uint64_t version_count = 0;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const Node& node : nodes_) {
      if (!FitsFixedString(node.key) || !FitsFixedString(node.metadata)) {
        return Status::InvalidArgument("HNSW key or metadata is too large");
      }
      if (node.vector_offset > vector_data_.size() ||
          vector_data_.size() - node.vector_offset < options_.dimension ||
          node.neighbors.size() >
              static_cast<size_t>(kMaximumRandomLevel + 1)) {
        return Status::Corruption("invalid HNSW node state");
      }
      AppendFixed64(&body, node.id);
      AppendFixed32(&body, static_cast<std::uint32_t>(node.key.size()));
      body.append(node.key);
      AppendFixed64(&body, node.sequence);
      const float* vector = VectorData(node.id);
      for (size_t dimension = 0; dimension < options_.dimension; ++dimension) {
        const float value = vector[dimension];
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        AppendFixed32(&body, bits);
      }
      AppendFixed32(&body, static_cast<std::uint32_t>(node.metadata.size()));
      body.append(node.metadata);
      AppendFixed32(&body, static_cast<std::uint32_t>(node.neighbors.size()));
      for (const auto& neighbors : node.neighbors) {
        if (neighbors.size() > std::numeric_limits<std::uint32_t>::max()) {
          return Status::InvalidArgument("HNSW neighbor list is too large");
        }
        AppendFixed32(&body, static_cast<std::uint32_t>(neighbors.size()));
        for (NodeId neighbor : neighbors) {
          AppendFixed64(&body, neighbor);
        }
      }
    }

    for (const auto& key_versions : versions_by_key_) {
      if (!FitsFixedString(key_versions.first) ||
          key_versions.second.size() > std::numeric_limits<std::uint64_t>::max() -
              version_count) {
        return Status::InvalidArgument("HNSW version state is too large");
      }
      version_count += key_versions.second.size();
    }
    for (const auto& key_versions : versions_by_key_) {
      for (const auto& version : key_versions.second) {
        AppendFixed32(&body,
                      static_cast<std::uint32_t>(key_versions.first.size()));
        body.append(key_versions.first);
        AppendFixed64(&body, version.first);
        AppendFixed64(&body, version.second.node_id);
        body.push_back(static_cast<char>(version.second.deleted ? 1 : 0));
      }
    }

    std::string file;
    AppendFixed64(&file, kIndexMagic);
    AppendFixed32(&file, kIndexVersion);
    AppendFixed32(&file, static_cast<std::uint32_t>(options_.dimension));
    AppendFixed32(&file, static_cast<std::uint32_t>(options_.max_neighbors));
    AppendFixed32(&file, static_cast<std::uint32_t>(options_.ef_construction));
    AppendFixed32(&file, static_cast<std::uint32_t>(options_.metric));
    AppendFixed32(&file, static_cast<std::uint32_t>(max_level_));
    AppendFixed64(&file, nodes_.size());
    AppendFixed64(&file, version_count);
    AppendFixed64(&file, entry_point_);
    file.append(body);
    AppendFixed32(&file, CRC32(body));

    const std::filesystem::path tmp_path = path.string() + ".tmp";
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path());
    }
    {
      std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        return Status::IOError("failed to open HNSW index temp file: " +
                               tmp_path.string());
      }
      output.write(file.data(), static_cast<std::streamsize>(file.size()));
      output.flush();
      if (!output) {
        return Status::IOError("failed to write HNSW index: " + path.string());
      }
    }
    status = FsyncFile(tmp_path);
    if (!status.ok()) {
      std::filesystem::remove(tmp_path);
      return status;
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
      std::filesystem::remove(tmp_path);
      return Status::IOError("failed to publish HNSW index: " + ec.message());
    }
    return FsyncDirectory(path.parent_path());
  }
}

Status HNSWIndex::Load(const std::filesystem::path& path) {
  Status options_status = ValidateOptions();
  if (!options_status.ok()) {
    return options_status;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return Status::IOError("failed to open HNSW index: " + path.string());
  }
  std::string file((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  if (file.size() < kIndexHeaderSize + kIndexFooterSize) {
    return Status::Corruption("HNSW index is truncated: " + path.string());
  }

  const std::string_view input_view(file);
  size_t offset = 0;
  std::uint64_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t dimension = 0;
  std::uint32_t max_neighbors = 0;
  std::uint32_t ef_construction = 0;
  std::uint32_t metric = 0;
  std::uint32_t encoded_max_level = 0;
  std::uint64_t node_count = 0;
  std::uint64_t version_count = 0;
  std::uint64_t entry_point = 0;
  Status status = ReadFixed64(input_view, &offset, &magic);
  if (!status.ok() || magic != kIndexMagic) {
    return Status::Corruption("invalid HNSW index magic");
  }
  status = ReadFixed32(input_view, &offset, &version);
  if (!status.ok() || (version != 1 && version != kIndexVersion)) {
    return Status::Corruption("unsupported HNSW index version");
  }
  if (!(ReadFixed32(input_view, &offset, &dimension).ok() &&
        ReadFixed32(input_view, &offset, &max_neighbors).ok() &&
        ReadFixed32(input_view, &offset, &ef_construction).ok() &&
        ReadFixed32(input_view, &offset, &metric).ok() &&
        ReadFixed32(input_view, &offset, &encoded_max_level).ok() &&
        ReadFixed64(input_view, &offset, &node_count).ok() &&
        ReadFixed64(input_view, &offset, &version_count).ok() &&
        ReadFixed64(input_view, &offset, &entry_point).ok())) {
    return Status::Corruption("truncated HNSW index header");
  }
  if (dimension != options_.dimension || max_neighbors != options_.max_neighbors ||
      ef_construction != options_.ef_construction ||
      metric != static_cast<std::uint32_t>(options_.metric)) {
    return Status::Corruption("HNSW index options do not match database options");
  }
  const int encoded_level = static_cast<std::int32_t>(encoded_max_level);
  if (encoded_level > kMaximumRandomLevel ||
      (node_count == 0 && encoded_level != -1) ||
      (node_count != 0 && encoded_level < 0) ||
      (node_count != 0 && entry_point >= node_count)) {
    return Status::Corruption("invalid HNSW index graph header");
  }

  if (node_count > std::numeric_limits<size_t>::max() / dimension) {
    return Status::Corruption("HNSW index vector data is too large");
  }
  const size_t body_size = file.size() - kIndexHeaderSize - kIndexFooterSize;
  if (node_count > body_size / (33ULL + 4ULL * dimension) ||
      version_count > body_size / 22) {
    return Status::Corruption("HNSW counts exceed file size");
  }
  std::vector<Node> loaded_nodes;
  loaded_nodes.reserve(static_cast<size_t>(node_count));
  std::vector<float> loaded_vector_data;
  loaded_vector_data.reserve(static_cast<size_t>(node_count) * dimension);
  for (std::uint64_t node_index = 0; node_index < node_count; ++node_index) {
    Node node;
    status = ReadFixed64(input_view, &offset, &node.id);
    if (!status.ok()) {
      return status;
    }
    if (node.id != node_index) {
      return Status::Corruption("HNSW node ids are not dense");
    }
    status = ReadString(input_view, &offset, &node.key);
    if (!status.ok() || node.key.empty()) {
      return Status::Corruption("invalid HNSW node key");
    }
    status = ReadFixed64(input_view, &offset, &node.sequence);
    if (!status.ok()) {
      return status;
    }
    node.vector_offset = loaded_vector_data.size();
    bool has_nonzero_value = false;
    for (std::uint32_t vector_index = 0; vector_index < dimension;
         ++vector_index) {
      std::uint32_t bits = 0;
      status = ReadFixed32(input_view, &offset, &bits);
      if (!status.ok()) {
        return status;
      }
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      if (!std::isfinite(value)) {
        return Status::Corruption("invalid vector in HNSW index");
      }
      has_nonzero_value = has_nonzero_value || value != 0.0f;
      loaded_vector_data.push_back(value);
    }
    if (options_.metric == VectorDistanceMetric::kCosine &&
        !has_nonzero_value) {
      return Status::Corruption("invalid vector in HNSW index");
    }
    status = ReadString(input_view, &offset, &node.metadata);
    if (!status.ok()) {
      return status;
    }
    std::uint32_t layer_count = 0;
    status = ReadFixed32(input_view, &offset, &layer_count);
    if (!status.ok() || layer_count == 0 ||
        layer_count > kMaximumRandomLevel + 1) {
      return Status::Corruption("invalid HNSW node layer count");
    }
    node.neighbors.resize(layer_count);
    for (size_t layer = 0; layer < node.neighbors.size(); ++layer) {
      auto& neighbors = node.neighbors[layer];
      std::uint32_t neighbor_count = 0;
      status = ReadFixed32(input_view, &offset, &neighbor_count);
      const size_t limit = version == 1 ? max_neighbors : NeighborLimit(layer);
      if (!status.ok() || neighbor_count > limit ||
          neighbor_count > (input_view.size() - offset) / sizeof(NodeId)) {
        return Status::Corruption("invalid HNSW neighbor count");
      }
      neighbors.resize(neighbor_count);
      for (NodeId& neighbor : neighbors) {
        status = ReadFixed64(input_view, &offset, &neighbor);
        if (!status.ok()) {
          return status;
        }
        if (neighbor >= node_count) {
          return Status::Corruption("HNSW neighbor id is out of range");
        }
      }
    }
    loaded_nodes.push_back(std::move(node));
  }

  std::unordered_map<std::string, std::map<SequenceNumber, VersionState>>
      loaded_versions;
  for (std::uint64_t i = 0; i < version_count; ++i) {
    std::string key;
    status = ReadString(input_view, &offset, &key);
    if (!status.ok() || key.empty()) {
      return Status::Corruption("invalid HNSW version key");
    }
    SequenceNumber sequence = 0;
    NodeId node_id = 0;
    status = ReadFixed64(input_view, &offset, &sequence);
    if (!status.ok()) {
      return status;
    }
    status = ReadFixed64(input_view, &offset, &node_id);
    if (!status.ok() || offset >= input_view.size()) {
      return Status::Corruption("truncated HNSW version state");
    }
    const unsigned char deletion_tag = input_view[offset++];
    if (deletion_tag > 1) {
      return Status::Corruption("invalid HNSW deletion tag");
    }
    const bool deleted = deletion_tag != 0;
    if (!deleted && node_id >= node_count) {
      return Status::Corruption("HNSW version node id is out of range");
    }
    auto& versions = loaded_versions[key];
    if (!versions.emplace(sequence, VersionState{node_id, deleted}).second) {
      return Status::Corruption("duplicate HNSW version state");
    }
  }
  if (offset + kIndexFooterSize != input_view.size()) {
    return Status::Corruption("trailing HNSW index data");
  }
  const std::uint32_t expected_checksum =
      DecodeFixed32(input_view.data() + offset);
  const size_t body_offset = kIndexHeaderSize;
  if (CRC32(input_view.substr(body_offset, offset - body_offset)) !=
      expected_checksum) {
    return Status::Corruption("HNSW index checksum mismatch");
  }

  int actual_max_level = -1;
  for (const auto& node : loaded_nodes) {
    actual_max_level = std::max(
        actual_max_level, static_cast<int>(node.neighbors.size()) - 1);
    auto versions_it = loaded_versions.find(node.key);
    if (versions_it == loaded_versions.end() ||
        versions_it->second.find(node.sequence) == versions_it->second.end() ||
        versions_it->second.at(node.sequence).deleted ||
        versions_it->second.at(node.sequence).node_id != node.id) {
      return Status::Corruption("HNSW node is missing version state");
    }
    for (size_t layer = 0; layer < node.neighbors.size(); ++layer) {
      auto neighbors = node.neighbors[layer];
      std::sort(neighbors.begin(), neighbors.end());
      if (std::adjacent_find(neighbors.begin(), neighbors.end()) !=
          neighbors.end()) {
        return Status::Corruption("duplicate HNSW neighbor");
      }
      for (NodeId neighbor : neighbors) {
        if (neighbor == node.id ||
            loaded_nodes[neighbor].neighbors.size() <= layer) {
          return Status::Corruption("invalid HNSW neighbor layer");
        }
      }
    }
  }
  if (actual_max_level != encoded_level ||
      (node_count != 0 && loaded_nodes[entry_point].neighbors.size() !=
                              static_cast<size_t>(encoded_level) + 1)) {
    return Status::Corruption("HNSW entry point does not cover graph layers");
  }
  for (const auto& key_versions : loaded_versions) {
    for (const auto& version_state : key_versions.second) {
      if (!version_state.second.deleted) {
        const auto& node = loaded_nodes[version_state.second.node_id];
        if (node.key != key_versions.first || node.sequence != version_state.first) {
          return Status::Corruption("HNSW version refers to a different node");
        }
      }
    }
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  nodes_ = std::move(loaded_nodes);
  vector_data_ = std::move(loaded_vector_data);
  versions_by_key_ = std::move(loaded_versions);
  max_level_ = node_count == 0 ? -1 : encoded_level;
  entry_point_ = node_count == 0 ? 0 : entry_point;
  return Status::OK();
}

Status HNSWIndex::MergeFrom(const std::filesystem::path& path) {
  HNSWIndex loaded(options_);
  Status status = loaded.Load(path);
  if (!status.ok()) {
    return status;
  }

  std::shared_lock<std::shared_mutex> lock(loaded.mutex_);
  for (const auto& key_versions : loaded.versions_by_key_) {
    for (const auto& version : key_versions.second) {
      if (version.second.deleted) {
        status = MarkDeleted(key_versions.first, version.first);
      } else {
        if (version.second.node_id >= loaded.nodes_.size()) {
          return Status::Corruption("HNSW version node id is out of range");
        }
        const Node& node = loaded.nodes_[version.second.node_id];
        const float* vector = loaded.VectorData(node.id);
        status = InsertVersion(key_versions.first, version.first,
                               std::vector<float>(
                                   vector, vector + loaded.options_.dimension),
                               node.metadata);
      }
      if (!status.ok()) {
        return status;
      }
    }
  }
  return Status::OK();
}

Status HNSWIndex::ReplaceWith(HNSWIndex&& other) {
  if (this == &other) {
    return Status::OK();
  }
  Status status = ValidateOptions();
  if (!status.ok()) {
    return status;
  }
  status = other.ValidateOptions();
  if (!status.ok()) {
    return status;
  }
  if (options_.dimension != other.options_.dimension ||
      options_.max_neighbors != other.options_.max_neighbors ||
      options_.ef_construction != other.options_.ef_construction ||
      options_.metric != other.options_.metric) {
    return Status::InvalidArgument("HNSW index options do not match");
  }

  std::unique_lock<std::shared_mutex> target_lock(mutex_, std::defer_lock);
  std::unique_lock<std::shared_mutex> source_lock(other.mutex_,
                                                  std::defer_lock);
  std::lock(target_lock, source_lock);
  nodes_ = std::move(other.nodes_);
  vector_data_ = std::move(other.vector_data_);
  versions_by_key_ = std::move(other.versions_by_key_);
  random_ = std::move(other.random_);
  level_multiplier_ = other.level_multiplier_;
  max_level_ = other.max_level_;
  entry_point_ = other.entry_point_;
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
  return SearchAtSequenceLocked(query, top_k, ef_search, read_sequence,
                                results);
}

Status HNSWIndex::SearchBatch(
    const std::vector<std::vector<float>>& queries,
    size_t top_k,
    size_t ef_search,
    std::vector<std::vector<VectorResult>>* results) const {
  return SearchBatchAtSequence(queries, top_k, ef_search,
                               std::numeric_limits<SequenceNumber>::max(),
                               results);
}

Status HNSWIndex::SearchBatchAtSequence(
    const std::vector<std::vector<float>>& queries,
    size_t top_k,
    size_t ef_search,
    SequenceNumber read_sequence,
    std::vector<std::vector<VectorResult>>* results) const {
  if (results == nullptr) {
    return Status::InvalidArgument("HNSW batch search results cannot be null");
  }
  results->clear();
  Status status = ValidateOptions();
  if (!status.ok()) {
    return status;
  }
  if (ef_search == 0) {
    return Status::InvalidArgument("HNSW ef_search must be greater than zero");
  }
  for (const auto& query : queries) {
    status = ValidateVector(query);
    if (!status.ok()) {
      return status;
    }
  }

  results->resize(queries.size());
  if (top_k == 0 || queries.empty()) {
    return Status::OK();
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
    status = SearchAtSequenceLocked(queries[query_index], top_k, ef_search,
                                    read_sequence, &(*results)[query_index]);
    if (!status.ok()) {
      results->clear();
      return status;
    }
  }
  return Status::OK();
}

Status HNSWIndex::SearchAtSequenceLocked(
    const std::vector<float>& query,
    size_t top_k,
    size_t ef_search,
    SequenceNumber read_sequence,
    std::vector<VectorResult>* results) const {
  results->clear();
  if (nodes_.empty()) {
    return Status::OK();
  }

  std::vector<NodeId> entry_points = {entry_point_};
  for (int level = max_level_; level > 0; --level) {
    const auto nearest = SearchLayer(query, entry_points, 1, level);
    if (nearest.empty()) {
      return Status::Corruption("HNSW search has no valid layer entry");
    }
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
