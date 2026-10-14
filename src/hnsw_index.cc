#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

#include "storage_util.h"

namespace kv {

namespace {

constexpr int kMaximumRandomLevel = 63;
constexpr std::uint64_t kIndexMagic = 0x4b56484e535731ULL;  // KVHNSW1
// Version 2 permits 2*M neighbors at level 0.
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

HNSWIndex::HNSWIndex(HNSWOptions options)
    : options_(std::move(options)), graph_(options_) {}

bool HNSWIndex::SequenceVisibility::IsVisible(HNSWNodeId node_id,
                                              const std::string& key) const {
  return index_->IsVisibleAt(node_id, key, read_sequence_);
}

Status HNSWIndex::ValidateOptions() const { return graph_.ValidateOptions(); }

Status HNSWIndex::ValidateVector(const std::vector<float>& vector) const {
  return graph_.ValidateVector(vector);
}

bool HNSWIndex::IsVisibleAt(NodeId node_id, const std::string& key,
                            SequenceNumber read_sequence) const {
  if (node_id >= graph_.Size()) {
    return false;
  }
  const auto versions_it = versions_by_key_.find(key);
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
      const NodeId node_id = existing->second.node_id;
      if (graph_.VectorEquals(node_id, vector) &&
          graph_.MetadataOf(node_id) == metadata) {
        return Status::OK();
      }
      return Status::InvalidArgument("HNSW key version already exists");
    }
  }

  NodeId node_id = 0;
  Status status = graph_.AddNode(vector, key, std::move(metadata), &node_id);
  if (!status.ok()) {
    return status;
  }
  versions_by_key_[std::move(key)].emplace(sequence,
                                           VersionState{node_id, false});
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

    // node_id -> sequence. The graph deliberately does not store sequence
    // numbers, so the on-disk node record recovers it from the version table.
    // The file format is unchanged by the stage-1 split.
    std::unordered_map<NodeId, SequenceNumber> sequence_of_node;
    sequence_of_node.reserve(graph_.Size());
    for (const auto& key_versions : versions_by_key_) {
      for (const auto& version : key_versions.second) {
        if (!version.second.deleted) {
          sequence_of_node.emplace(version.second.node_id, version.first);
        }
      }
    }
    if (sequence_of_node.size() != graph_.Size()) {
      return Status::Corruption("HNSW graph node is missing version state");
    }

    for (NodeId node_id = 0; node_id < graph_.Size(); ++node_id) {
      const std::string& key = graph_.KeyOf(node_id);
      const std::string& metadata = graph_.MetadataOf(node_id);
      const size_t layer_count = graph_.LevelCount(node_id);
      if (!FitsFixedString(key) || !FitsFixedString(metadata)) {
        return Status::InvalidArgument("HNSW key or metadata is too large");
      }
      if (layer_count > static_cast<size_t>(kMaximumRandomLevel + 1)) {
        return Status::Corruption("invalid HNSW node state");
      }
      AppendFixed64(&body, node_id);
      AppendFixed32(&body, static_cast<std::uint32_t>(key.size()));
      body.append(key);
      AppendFixed64(&body, sequence_of_node.at(node_id));
      const float* vector = graph_.VectorOf(node_id);
      for (size_t dimension = 0; dimension < options_.dimension; ++dimension) {
        const float value = vector[dimension];
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        AppendFixed32(&body, bits);
      }
      AppendFixed32(&body, static_cast<std::uint32_t>(metadata.size()));
      body.append(metadata);
      AppendFixed32(&body, static_cast<std::uint32_t>(layer_count));
      for (int level = 0; level < static_cast<int>(layer_count); ++level) {
        const auto& neighbors = graph_.NeighborsOf(node_id, level);
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
    AppendFixed32(&file, static_cast<std::uint32_t>(graph_.MaxLevel()));
    AppendFixed64(&file, graph_.Size());
    AppendFixed64(&file, version_count);
    AppendFixed64(&file, graph_.EntryPoint());
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
  if (!status.ok() || version != kIndexVersion) {
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
  std::vector<HNSWGraphNode> loaded_nodes;
  loaded_nodes.reserve(static_cast<size_t>(node_count));
  std::vector<SequenceNumber> loaded_sequences;
  loaded_sequences.reserve(static_cast<size_t>(node_count));
  std::vector<float> loaded_vector_data;
  loaded_vector_data.reserve(static_cast<size_t>(node_count) * dimension);
  for (std::uint64_t node_index = 0; node_index < node_count; ++node_index) {
    std::uint64_t node_id = 0;
    status = ReadFixed64(input_view, &offset, &node_id);
    if (!status.ok()) {
      return status;
    }
    if (node_id != node_index) {
      return Status::Corruption("HNSW node ids are not dense");
    }
    HNSWGraphNode node;
    status = ReadString(input_view, &offset, &node.key);
    if (!status.ok() || node.key.empty()) {
      return Status::Corruption("invalid HNSW node key");
    }
    SequenceNumber sequence = 0;
    status = ReadFixed64(input_view, &offset, &sequence);
    if (!status.ok()) {
      return status;
    }
    loaded_sequences.push_back(sequence);
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
      const size_t limit = graph_.NeighborLimit(layer);
      if (!status.ok() || neighbor_count > limit ||
          neighbor_count > (input_view.size() - offset) / sizeof(HNSWNodeId)) {
        return Status::Corruption("invalid HNSW neighbor count");
      }
      neighbors.resize(neighbor_count);
      for (HNSWNodeId& neighbor : neighbors) {
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

  // Everything below runs before any state is published, so a corrupt file
  // cannot partially replace a live graph.
  int actual_max_level = -1;
  for (size_t index = 0; index < loaded_nodes.size(); ++index) {
    const HNSWGraphNode& node = loaded_nodes[index];
    actual_max_level = std::max(
        actual_max_level, static_cast<int>(node.neighbors.size()) - 1);
    auto versions_it = loaded_versions.find(node.key);
    if (versions_it == loaded_versions.end() ||
        versions_it->second.find(loaded_sequences[index]) ==
            versions_it->second.end() ||
        versions_it->second.at(loaded_sequences[index]).deleted ||
        versions_it->second.at(loaded_sequences[index]).node_id != index) {
      return Status::Corruption("HNSW node is missing version state");
    }
    for (size_t layer = 0; layer < node.neighbors.size(); ++layer) {
      auto neighbors = node.neighbors[layer];
      std::sort(neighbors.begin(), neighbors.end());
      if (std::adjacent_find(neighbors.begin(), neighbors.end()) !=
          neighbors.end()) {
        return Status::Corruption("duplicate HNSW neighbor");
      }
      for (HNSWNodeId neighbor : neighbors) {
        if (neighbor == index ||
            loaded_nodes[neighbor].neighbors.size() <= layer) {
          return Status::Corruption("invalid HNSW neighbor layer");
        }
      }
    }
  }
  if (actual_max_level != encoded_level ||
      (node_count != 0 &&
       loaded_nodes[entry_point].neighbors.size() !=
           static_cast<size_t>(encoded_level) + 1)) {
    return Status::Corruption("HNSW entry point does not cover graph layers");
  }
  for (const auto& key_versions : loaded_versions) {
    for (const auto& version_state : key_versions.second) {
      if (!version_state.second.deleted) {
        const HNSWGraphNode& node = loaded_nodes[version_state.second.node_id];
        if (node.key != key_versions.first ||
            loaded_sequences[version_state.second.node_id] !=
                version_state.first) {
          return Status::Corruption("HNSW version refers to a different node");
        }
      }
    }
  }

  status = graph_.Replace(std::move(loaded_nodes), std::move(loaded_vector_data),
                          static_cast<HNSWNodeId>(entry_point), encoded_level);
  if (!status.ok()) {
    return status;
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  versions_by_key_ = std::move(loaded_versions);
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
        if (version.second.node_id >= loaded.graph_.Size()) {
          return Status::Corruption("HNSW version node id is out of range");
        }
        const float* vector = loaded.graph_.VectorOf(version.second.node_id);
        status = InsertVersion(
            key_versions.first, version.first,
            std::vector<float>(vector, vector + loaded.options_.dimension),
            loaded.graph_.MetadataOf(version.second.node_id));
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
  graph_ = std::move(other.graph_);
  versions_by_key_ = std::move(other.versions_by_key_);
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
  if (graph_.Size() == 0) {
    return Status::OK();
  }

  const SequenceVisibility visibility(this, read_sequence);
  std::vector<HNSWCandidate> candidates;
  Status status =
      graph_.Search(query, top_k, ef_search, &visibility, &candidates);
  if (!status.ok()) {
    return status;
  }

  results->reserve(candidates.size());
  for (const HNSWCandidate& candidate : candidates) {
    results->push_back({graph_.KeyOf(candidate.id), candidate.distance,
                        graph_.MetadataOf(candidate.id)});
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
  return graph_.Size();
}

HNSWStats HNSWIndex::Stats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return graph_.Stats();
}

}  // namespace kv
