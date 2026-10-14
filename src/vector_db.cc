#include "vector_db.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <mutex>
#include <queue>
#include <utility>

namespace kv {
namespace {

struct ResultBetter {
  bool operator()(const VectorResult& lhs, const VectorResult& rhs) const {
    return lhs.distance != rhs.distance ? lhs.distance < rhs.distance
                                        : lhs.key < rhs.key;
  }
};

}  // namespace

std::string VectorWriteResult::ToString() const {
  const char* commit = commit_state == CommitState::Committed ? "committed"
                     : commit_state == CommitState::Unknown ? "unknown" : "not committed";
  return std::string("KV ") + commit + ": " + kv_status.ToString() +
         "; index: " + index_status.ToString();
}

Status VectorWriteResult::status() const {
  if (ok()) return Status::OK();
  if (commit_state == CommitState::NotCommitted) return kv_status;
  return Status::IOError(ToString());
}

VectorDB::VectorDB(VectorOptions options)
    : options_(std::move(options)), db_(options_.kv_options) {}
VectorDB::~VectorDB() { (void)Close(); }

HNSWOptions VectorDB::IndexOptions() const {
  HNSWOptions options;
  options.dimension = options_.vector_dimension;
  options.max_neighbors = options_.hnsw_max_neighbors;
  options.ef_construction = options_.hnsw_ef_construction;
  options.metric = options_.vector_distance_metric;
  return options;
}

Status VectorDB::CheckOpen() const {
  return state_ == IndexState::Closed || state_ == IndexState::Opening
      ? Status::IOError("vector database is not open") : Status::OK();
}

Status VectorDB::ValidateVector(const std::vector<float>& vector,
                                VectorDistanceMetric metric) const {
  if (vector.size() != options_.vector_dimension) {
    return Status::InvalidArgument("vector dimension does not match configured dimension");
  }
  float distance = 0;
  return ComputeVectorDistance(vector, vector, metric, &distance);
}

Status VectorDB::ValidateBatch(const WriteBatch& batch) const {
  for (const auto& operation : batch.Operations()) {
    if (operation.key.empty() || operation.key.find_first_of("\t\n") != std::string::npos ||
        operation.key.find('\0') != std::string::npos) {
      return Status::InvalidArgument("key cannot be empty or contain tab, newline, or NUL");
    }
    if (operation.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        operation.value.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status::InvalidArgument("batch key or value exceeds uint32 range");
    }
    if (operation.type == WriteBatchOpType::kPut && IsEncodedVectorValue(operation.value)) {
      VectorRecord record;
      Status status = DecodeVectorValue(operation.value, &record);
      if (!status.ok()) return status;
      status = ValidateVector(record.vector, options_.vector_distance_metric);
      if (!status.ok()) return status;
    } else if (operation.type != WriteBatchOpType::kPut &&
               operation.type != WriteBatchOpType::kDelete) {
      return Status::InvalidArgument("unknown batch operation");
    }
  }
  // Validate the same payload bound as WAL before its commit boundary.
  return batch.ValidateEncoding();
}

Status VectorDB::Open() {
  std::unique_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::unique_lock<std::shared_mutex> coordination(coordination_mu_);
  if (state_ != IndexState::Closed) return Status::InvalidArgument("database is already open");
  Status status = HNSWGraph(IndexOptions()).ValidateOptions();
  if (!status.ok()) return status;
  state_ = IndexState::Opening;
  try {
    status = db_.Open();
    if (status.ok()) status = RebuildIndexLocked();
  } catch (const std::exception& error) {
    status = Status::IOError(error.what());
  }
  if (!status.ok()) {
    (void)db_.Close();
    index_.reset();
    state_ = IndexState::Closed;
    index_status_ = status;
  }
  return status;
}

Status VectorDB::Close() {
  std::unique_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::unique_lock<std::shared_mutex> coordination(coordination_mu_);
  Status status = db_.Close();
  index_.reset();
  state_ = IndexState::Closed;
  index_status_ = Status::OK();
  return status;
}

void VectorDB::MarkDirty(Status status) {
  state_ = IndexState::Dirty;
  index_status_ = std::move(status);
}

Status VectorDB::BuildIndex(const ReadOptions& options,
                            std::unique_ptr<HNSWIndex>* index) {
  // Iterator pins one KV view. Coordination excludes extension writes and
  // snapshot release; background compaction may continue on pinned files.
  auto iterator = db_.NewIterator(options);
  if (!iterator->status().ok()) return iterator->status();
  auto replacement = std::make_unique<HNSWIndex>(IndexOptions());
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    if (!IsEncodedVectorValue(iterator->value())) continue;
    VectorRecord record;
    Status status = DecodeVectorValue(iterator->value(), &record);
    if (!status.ok()) return status;
    status = ValidateVector(record.vector, options_.vector_distance_metric);
    if (!status.ok()) return Status::Corruption("invalid stored vector: " + status.ToString());
    status = replacement->Insert(iterator->key(), record.vector, record.metadata);
    if (!status.ok()) return status;
  }
  if (!iterator->status().ok()) return iterator->status();
  *index = std::move(replacement);
  return Status::OK();
}

Status VectorDB::RebuildIndexLocked() {
  state_ = IndexState::Rebuilding;
  Status status;
  try {
    if (options_.testing_before_rebuild) status = options_.testing_before_rebuild();
    std::unique_ptr<HNSWIndex> replacement;
    if (status.ok()) status = BuildIndex({}, &replacement);
    if (status.ok()) {
      index_ = std::move(replacement);
      index_sequence_ = 0;
      operations_since_rebuild_ = 0;
      index_status_ = Status::OK();
      state_ = IndexState::Ready;
      return status;
    }
  } catch (const std::exception& error) {
    status = Status::IOError(error.what());
  }
  MarkDirty(status);
  return status;
}

Status VectorDB::RebuildIndex() {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::unique_lock<std::shared_mutex> coordination(coordination_mu_);
  Status status = CheckOpen();
  return status.ok() ? RebuildIndexLocked() : status;
}

VectorWriteResult VectorDB::Write(const VectorWriteBatch& batch) {
  return Write(batch.KVBatch());
}

VectorWriteResult VectorDB::Write(const WriteBatch& batch) {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::unique_lock<std::shared_mutex> coordination(coordination_mu_);
  Status status = CheckOpen();
  if (status.ok()) status = ValidateBatch(batch);
  if (!status.ok()) return {status, Status::OK(), CommitState::NotCommitted};
  try {
    status = db_.Write(batch);
  } catch (const std::exception& error) {
    status = Status::IOError(error.what());
  }
  if (!status.ok()) {
    MarkDirty(Status::IOError("KV commit outcome unknown; rebuild before indexed search"));
    return {status, index_status_, CommitState::Unknown};
  }
  if (state_ == IndexState::Dirty) {
    return {Status::OK(), index_status_, CommitState::Committed};
  }
  try {
    if (options_.testing_after_kv_commit) options_.testing_after_kv_commit();
    size_t position = 0;
    for (const auto& operation : batch.Operations()) {
      if (options_.testing_before_index_update) {
        status = options_.testing_before_index_update(position);
        if (!status.ok()) break;
      }
      ++position;
      ++index_sequence_;
      if (operation.type == WriteBatchOpType::kPut && IsEncodedVectorValue(operation.value)) {
        VectorRecord record;
        status = DecodeVectorValue(operation.value, &record);
        if (status.ok()) status = index_->InsertVersion(
            operation.key, index_sequence_, record.vector, record.metadata);
      } else {
        status = index_->MarkDeleted(operation.key, index_sequence_);
      }
      if (!status.ok()) break;
    }
    if (status.ok()) {
      operations_since_rebuild_ += batch.Count();
      if (options_.index_rebuild_operations != 0 &&
          operations_since_rebuild_ >= options_.index_rebuild_operations) {
        status = RebuildIndexLocked();
      }
    }
  } catch (const std::exception& error) {
    status = Status::IOError(error.what());
  }
  if (!status.ok()) MarkDirty(status);
  return {Status::OK(), index_status_, CommitState::Committed};
}

VectorWriteResult VectorDB::PutVector(const std::string& key,
                                      const std::vector<float>& vector,
                                      const std::string& metadata) {
  VectorWriteBatch batch;
  Status status = batch.PutVector(key, vector, metadata);
  if (!status.ok()) return {status, Status::OK(), CommitState::NotCommitted};
  return Write(batch);
}

Status VectorDB::Put(const std::string& key, const std::string& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(batch).status();
}

Status VectorDB::Delete(const std::string& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(batch).status();
}

Status VectorDB::Get(const std::string& key, std::string* value,
                     const ReadOptions& options) {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  Status status = CheckOpen();
  return status.ok() ? db_.Get(key, value, options) : status;
}

Status VectorDB::GetVector(const std::string& key, VectorRecord* record,
                           const ReadOptions& options) {
  if (record == nullptr) return Status::InvalidArgument("vector record output cannot be null");
  std::string encoded;
  Status status = Get(key, &encoded, options);
  if (status.ok()) status = DecodeVectorValue(encoded, record);
  if (status.ok()) status = ValidateVector(record->vector, options_.vector_distance_metric);
  if (status.ok()) record->key = key;
  return status;
}

Status VectorDB::Search(const std::vector<float>& query, size_t top_k,
                        size_t ef_search, std::vector<VectorResult>* results,
                        const ReadOptions& options) {
  if (results == nullptr) return Status::InvalidArgument("search results cannot be null");
  results->clear();
  std::vector<std::vector<VectorResult>> batch_results;
  Status status = SearchBatch({query}, top_k, ef_search, &batch_results, options);
  if (status.ok()) *results = std::move(batch_results.front());
  return status;
}

Status VectorDB::Search(const std::vector<float>& query, size_t top_k,
                        std::vector<VectorResult>* results,
                        const ReadOptions& options) {
  return Search(query, top_k, 64, results, options);
}

Status VectorDB::SearchBatch(const std::vector<std::vector<float>>& queries,
                             size_t top_k, size_t ef_search,
                             std::vector<std::vector<VectorResult>>* results,
                             const ReadOptions& options) {
  if (results == nullptr) return Status::InvalidArgument("batch search results cannot be null");
  results->clear();
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  Status status = CheckOpen();
  if (!status.ok()) return status;
  if (state_ != IndexState::Ready) {
    return Status::IOError("index unavailable; RebuildIndex required: " + index_status_.ToString());
  }
  if (ef_search == 0) return Status::InvalidArgument("ef_search must be greater than zero");
  for (const auto& query : queries) {
    status = ValidateVector(query, options_.vector_distance_metric);
    if (!status.ok()) return status;
  }
  std::unique_ptr<HNSWIndex> historical;
  HNSWIndex* selected = index_.get();
  if (options.snapshot != nullptr) {
    status = BuildIndex(options, &historical);
    if (!status.ok()) return status;
    selected = historical.get();
  }
  return selected->SearchBatch(queries, top_k, ef_search, results);
}

Status VectorDB::BruteForceSearch(const std::vector<float>& query, size_t top_k,
                                  std::vector<VectorResult>* results,
                                  VectorDistanceMetric metric,
                                  const ReadOptions& options) {
  if (results == nullptr) return Status::InvalidArgument("search results cannot be null");
  results->clear();
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  Status status = CheckOpen();
  if (status.ok()) status = ValidateVector(query, metric);
  if (!status.ok()) return status;
  auto iterator = db_.NewIterator(options);
  if (!iterator->status().ok()) return iterator->status();
  if (top_k == 0) return Status::OK();
  std::priority_queue<VectorResult, std::vector<VectorResult>, ResultBetter> nearest;
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    if (!IsEncodedVectorValue(iterator->value())) continue;
    VectorRecord record;
    status = DecodeVectorValue(iterator->value(), &record);
    if (!status.ok()) return status;
    if (record.vector.size() != options_.vector_dimension) {
      return Status::Corruption("stored vector dimension does not match configured dimension");
    }
    float distance = 0;
    status = ComputeVectorDistance(query, record.vector, metric, &distance);
    if (!status.ok()) return status;
    VectorResult candidate{iterator->key(), distance, std::move(record.metadata)};
    if (nearest.size() < top_k) nearest.push(std::move(candidate));
    else if (ResultBetter{}(candidate, nearest.top())) {
      nearest.pop(); nearest.push(std::move(candidate));
    }
  }
  if (!iterator->status().ok()) return iterator->status();
  while (!nearest.empty()) { results->push_back(nearest.top()); nearest.pop(); }
  std::sort(results->begin(), results->end(), ResultBetter{});
  return Status::OK();
}

IndexState VectorDB::GetIndexState() const {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  return state_;
}
Status VectorDB::GetIndexStatus() const {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  return index_status_;
}
HNSWStats VectorDB::VectorIndexStats() const {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  return index_ == nullptr ? HNSWStats{} : index_->Stats();
}
Status VectorDB::Compact() {
  // Core compaction changes storage only; keep it independent of coordination.
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  return db_.Compact();
}
DBStats VectorDB::Stats() const {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  return db_.Stats();
}
const Snapshot* VectorDB::GetSnapshot() {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  return db_.GetSnapshot();
}
void VectorDB::ReleaseSnapshot(const Snapshot* snapshot) {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::unique_lock<std::shared_mutex> coordination(coordination_mu_);
  db_.ReleaseSnapshot(snapshot);
}
std::unique_ptr<Iterator> VectorDB::NewIterator(const ReadOptions& options) {
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mu_);
  std::shared_lock<std::shared_mutex> coordination(coordination_mu_);
  return db_.NewIterator(options);
}

}  // namespace kv
