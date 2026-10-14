#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>

#include "db.h"
#include "hnsw_index.h"
#include "vector_write_batch.h"

namespace kv {

struct VectorOptions {
  Options kv_options;
  size_t vector_dimension = 0;
  size_t hnsw_max_neighbors = 16;
  size_t hnsw_ef_construction = 200;
  VectorDistanceMetric vector_distance_metric = VectorDistanceMetric::kL2;
  // Bound obsolete nodes AND tombstones independently of KV compaction.
  // Zero disables automatic reclamation; RebuildIndex remains available.
  size_t index_rebuild_operations = 4096;
  // Fault injection hooks run under write coordination; must not reenter DB.
  std::function<void()> testing_after_kv_commit;
  std::function<Status(size_t)> testing_before_index_update;
  std::function<Status()> testing_before_rebuild;
};

enum class IndexState { Closed, Opening, Ready, Dirty, Rebuilding };
enum class CommitState { NotCommitted, Committed, Unknown };

struct VectorWriteResult {
  Status kv_status;
  Status index_status;
  CommitState commit_state = CommitState::NotCommitted;

  bool ok() const { return committed() && kv_status.ok() && index_status.ok(); }
  bool committed() const { return commit_state == CommitState::Committed; }
  std::string ToString() const;
  // Used only by ordinary Put/Delete convenience calls. An index failure
  // explicitly says that KV committed. Write/PutVector retain both statuses.
  Status status() const;
};

// Owns the only writable KV handle. Lock order: lifecycle -> coordination ->
// either KV or HNSW. Core maintenance never calls back into this layer.
class VectorDB {
 public:
  explicit VectorDB(VectorOptions options);
  ~VectorDB();
  VectorDB(const VectorDB&) = delete;
  VectorDB& operator=(const VectorDB&) = delete;

  Status Open();
  Status Close();
  VectorWriteResult Write(const VectorWriteBatch& batch);
  VectorWriteResult Write(const WriteBatch& batch);
  VectorWriteResult PutVector(const std::string& key,
                              const std::vector<float>& vector,
                              const std::string& metadata = {});
  Status Put(const std::string& key, const std::string& value);
  Status Delete(const std::string& key);
  Status Get(const std::string& key, std::string* value,
             const ReadOptions& options = {});
  Status GetVector(const std::string& key, VectorRecord* record,
                   const ReadOptions& options = {});
  Status BruteForceSearch(const std::vector<float>& query, size_t top_k,
                          std::vector<VectorResult>* results,
                          VectorDistanceMetric metric = VectorDistanceMetric::kL2,
                          const ReadOptions& options = {});
  Status Search(const std::vector<float>& query, size_t top_k, size_t ef_search,
                std::vector<VectorResult>* results,
                const ReadOptions& options = {});
  Status Search(const std::vector<float>& query, size_t top_k,
                std::vector<VectorResult>* results,
                const ReadOptions& options = {});
  Status SearchBatch(const std::vector<std::vector<float>>& queries,
                     size_t top_k, size_t ef_search,
                     std::vector<std::vector<VectorResult>>* results,
                     const ReadOptions& options = {});
  Status RebuildIndex();
  IndexState GetIndexState() const;
  Status GetIndexStatus() const;
  HNSWStats VectorIndexStats() const;
  Status Compact();
  DBStats Stats() const;
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot* snapshot);
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options = {});

 private:
  HNSWOptions IndexOptions() const;
  Status CheckOpen() const;
  Status ValidateVector(const std::vector<float>& vector,
                        VectorDistanceMetric metric) const;
  Status ValidateBatch(const WriteBatch& batch) const;
  Status BuildIndex(const ReadOptions& options,
                    std::unique_ptr<HNSWIndex>* index);
  Status RebuildIndexLocked();
  void MarkDirty(Status status);

  VectorOptions options_;
  DB db_;
  std::unique_ptr<HNSWIndex> index_;
  IndexState state_ = IndexState::Closed;
  Status index_status_;
  SequenceNumber index_sequence_ = 0;
  size_t operations_since_rebuild_ = 0;
  mutable std::shared_mutex lifecycle_mu_;
  mutable std::shared_mutex coordination_mu_;
};

}  // namespace kv
