#include "vector_db.h"
#include "test_support.h"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

void MustOK(const kv::VectorWriteResult& result) {
  if (!result.ok()) { std::cerr << result.ToString() << '\n'; std::abort(); }
}

kv::VectorOptions Options(const std::string& name) {
  kv::VectorOptions options;
  options.kv_options.db_path = TestDBPath(name);
  options.kv_options.memtable_entries_limit = 8;
  options.kv_options.level0_sstable_limit = 2;
  options.vector_dimension = 3;
  return options;
}

void TestAtomicMixedBatchesAndValidation() {
  auto options = Options("extension_batches");
  // Keep this WAL live while inspecting it; Flush may otherwise remove it.
  options.kv_options.memtable_entries_limit = 1000;
  std::filesystem::remove_all(options.kv_options.db_path);
  kv::VectorDB db(options); MustOK(db.Open());
  kv::VectorWriteBatch batch;
  MustOK(batch.PutVector("a", {1,0,0}, "first"));
  batch.Put("a", "ordinary"); batch.Delete("a");
  MustOK(batch.PutVector("a", {2,0,0}, "last"));
  MustOK(batch.PutVector("b", {0,1,0})); batch.Put("b", "plain");
  MustOK(batch.PutVector("c", {0,0,1})); batch.Delete("c");
  const auto result = db.Write(batch);
  CHECK(result.committed()); MustOK(result);
  kv::VectorRecord record; MustOK(db.GetVector("a", &record)); CHECK(record.metadata == "last");
  CHECK(!db.GetVector("b", &record).ok()); CHECK(db.GetVector("c", &record).IsNotFound());
  std::vector<kv::VectorResult> results;
  MustOK(db.Search({2,0,0}, 10, &results)); CHECK(results.size() == 1); CHECK(results[0].key == "a");
  // The live WAL must contain only ordinary Put/Delete tags, even for vectors.
  kv::WALReader reader(options.kv_options.db_path / "wal_000001.log");
  kv::WALBatchRecord wal; bool eof = false; MustOK(reader.ReadNext(&wal, &eof));
  CHECK(!eof); CHECK(wal.batch.Count() == batch.Count());
  for (const auto& operation : wal.batch.Operations()) {
    CHECK(operation.type == kv::WriteBatchOpType::kPut || operation.type == kv::WriteBatchOpType::kDelete);
  }
  const auto* before = db.GetSnapshot();
  const auto wal_size = std::filesystem::file_size(options.kv_options.db_path / "wal_000001.log");
  for (int mode = 0; mode < 4; ++mode) {
    kv::VectorWriteBatch bad;
    bad.Put("must-not-commit", "x");
    if (mode == 0) MustOK(bad.PutVector("dimension", {1,2}));
    if (mode == 1) bad.Put("bad-encoding", "KVV1broken");
    if (mode == 2) bad.Put("", "bad-key");
    if (mode == 3) bad.Put(std::string("nul\0key", 7), "bad-key");
    const auto rejected = db.Write(bad);
    CHECK(!rejected.ok()); CHECK(rejected.commit_state == kv::CommitState::NotCommitted);
  }
  CHECK(!db.PutVector("nan", {std::numeric_limits<float>::quiet_NaN(),0,0}).ok());
  CHECK(!db.PutVector("inf", {std::numeric_limits<float>::infinity(),0,0}).ok());
  const auto* after = db.GetSnapshot(); CHECK(after->sequence() == before->sequence());
  db.ReleaseSnapshot(before); db.ReleaseSnapshot(after);
  CHECK(std::filesystem::file_size(options.kv_options.db_path / "wal_000001.log") == wal_size);
  std::string value; CHECK(db.Get("must-not-commit", &value).IsNotFound());
  CHECK(db.GetIndexState() == kv::IndexState::Ready);
  MustOK(db.Close()); std::filesystem::remove_all(options.kv_options.db_path);

  options.vector_distance_metric = kv::VectorDistanceMetric::kCosine;
  kv::VectorDB cosine(options); MustOK(cosine.Open());
  CHECK(!cosine.PutVector("zero", {0,0,0}).ok());
  MustOK(cosine.PutVector("unit", {1,0,0}));
  MustOK(cosine.Close()); std::filesystem::remove_all(options.kv_options.db_path);
}

void TestCommittedIndexFailureAndRebuild() {
  auto options = Options("extension_dirty");
  std::filesystem::remove_all(options.kv_options.db_path);
  bool fail_update = false, fail_rebuild = false, throw_update = false;
  options.testing_before_index_update = [&](size_t position) {
    if (throw_update) throw std::runtime_error("injected allocation/algorithm failure");
    return fail_update && position == 1 ? kv::Status::IOError("injected index failure") : kv::Status::OK();
  };
  options.testing_before_rebuild = [&] {
    return fail_rebuild ? kv::Status::IOError("injected rebuild failure") : kv::Status::OK();
  };
  kv::VectorDB db(options); MustOK(db.Open()); MustOK(db.PutVector("old", {1,0,0}));
  fail_update = true;
  kv::VectorWriteBatch batch;
  batch.Delete("old"); MustOK(batch.PutVector("a", {0,1,0})); MustOK(batch.PutVector("b", {0,0,1}));
  auto result = db.Write(batch);
  CHECK(result.committed()); CHECK(result.kv_status.ok()); CHECK(!result.index_status.ok()); CHECK(!result.ok());
  CHECK(result.ToString().find("committed") != std::string::npos);
  CHECK(db.GetIndexState() == kv::IndexState::Dirty); CHECK(!db.GetIndexStatus().ok());
  kv::VectorRecord record; MustOK(db.GetVector("a", &record)); MustOK(db.GetVector("b", &record));
  CHECK(db.GetVector("old", &record).IsNotFound());
  std::vector<kv::VectorResult> results{{"stale",0,{}}};
  CHECK(!db.Search({1,0,0}, 3, &results).ok()); CHECK(results.empty());
  std::vector<std::vector<kv::VectorResult>> batched(2);
  CHECK(!db.SearchBatch({{1,0,0}}, 3, 32, &batched).ok()); CHECK(batched.empty());
  MustOK(db.BruteForceSearch({0,1,0}, 3, &results)); CHECK(results.size() == 2);
  result = db.PutVector("while-dirty", {2,2,2}); CHECK(result.committed()); CHECK(!result.index_status.ok());
  MustOK(db.GetVector("while-dirty", &record));
  MustOK(db.Compact()); // Index errors never enter core background_status.
  fail_rebuild = true; CHECK(!db.RebuildIndex().ok()); CHECK(db.GetIndexState() == kv::IndexState::Dirty);
  fail_rebuild = false; fail_update = false;
  MustOK(db.RebuildIndex()); CHECK(db.GetIndexState() == kv::IndexState::Ready);
  MustOK(db.Search({0,1,0}, 10, &results)); CHECK(results.size() == 3);
  throw_update = true; result = db.PutVector("thrown", {3,3,3});
  CHECK(result.committed()); CHECK(!result.index_status.ok()); CHECK(db.GetIndexState() == kv::IndexState::Dirty);
  throw_update = false; MustOK(db.RebuildIndex());
  MustOK(db.Close()); MustOK(db.Open());
  MustOK(db.Search({3,3,3}, 1, &results)); CHECK(results[0].key == "thrown");
  MustOK(db.Close()); std::filesystem::remove_all(options.kv_options.db_path);
}

void TestUnknownKVOutcome() {
  auto options = Options("extension_unknown");
  std::filesystem::remove_all(options.kv_options.db_path);
  options.kv_options.testing_fail_flush = true;
  options.kv_options.memtable_entries_limit = 1;
  {
    kv::VectorDB db(options); MustOK(db.Open());
    MustOK(db.PutVector("durable", {1,0,0}));
    // The asynchronous flush must report its injected failure before this write.
    kv::VectorWriteResult failed;
    bool observed = false;
    for (int i = 0; i < 200 && !observed; ++i) {
      failed = db.PutVector("later", {0,1,0});
      observed = !failed.kv_status.ok();
      if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(observed); CHECK(failed.commit_state == kv::CommitState::Unknown);
    CHECK(db.GetIndexState() == kv::IndexState::Dirty);
    std::vector<kv::VectorResult> results; CHECK(!db.Search({1,0,0}, 1, &results).ok());
    CHECK(!db.Close().ok());
  }
  options.kv_options.testing_fail_flush = false;
  kv::VectorDB recovered(options); MustOK(recovered.Open());
  kv::VectorRecord record; MustOK(recovered.GetVector("durable", &record));
  MustOK(recovered.Close()); std::filesystem::remove_all(options.kv_options.db_path);
}

void TestLifecycleAndSnapshotOwnership() {
  auto options = Options("extension_lifecycle");
  std::filesystem::remove_all(options.kv_options.db_path);
  bool fail_open = true;
  options.testing_before_rebuild = [&] {
    return fail_open ? kv::Status::IOError("injected open failure") : kv::Status::OK();
  };
  kv::VectorDB db(options);
  std::vector<kv::VectorResult> results;
  std::string value;
  CHECK(db.GetSnapshot() == nullptr); CHECK(!db.Get("a", &value).ok());
  CHECK(!db.Search({1,0,0}, 1, &results).ok()); CHECK(!db.Write(kv::VectorWriteBatch{}).ok());
  CHECK(!db.NewIterator()->status().ok()); CHECK(!db.RebuildIndex().ok());
  CHECK(!db.Open().ok()); CHECK(db.GetIndexState() == kv::IndexState::Closed);
  fail_open = false; MustOK(db.Open()); CHECK(!db.Open().ok());
  MustOK(db.PutVector("a", {1,0,0}, "historical"));
  MustOK(db.PutVector("b", {0,1,0}));
  const auto* historical = db.GetSnapshot();
  kv::ReadOptions read{historical};
  MustOK(db.Put("a", "ordinary")); MustOK(db.Delete("b"));
  for (int i = 0; i < 32; ++i) MustOK(db.PutVector("current", {float(i),0,1}));
  MustOK(db.Search({1,0,0}, 2, &results, read)); CHECK(results.size() == 2);
  CHECK(results[0].key == "a"); CHECK(results[0].metadata == "historical");
  MustOK(db.Compact()); MustOK(db.RebuildIndex());
  MustOK(db.Search({1,0,0}, 2, &results, read)); CHECK(results.size() == 2); CHECK(results[0].key == "a");
  std::vector<std::vector<kv::VectorResult>> batch;
  MustOK(db.SearchBatch({{1,0,0},{0,1,0}}, 2, 32, &batch, read));
  CHECK(batch.size() == 2); CHECK(batch[0][0].key == "a"); CHECK(batch[1][0].key == "b");
  db.ReleaseSnapshot(historical);
  const auto* fresh = db.GetSnapshot(); CHECK(fresh != historical);
  CHECK(!db.Search({1,0,0}, 2, &results, read).ok()); CHECK(results.empty());
  CHECK(!db.Get("a", &value, read).ok()); CHECK(!db.NewIterator(read)->status().ok());
  auto other_options = Options("extension_foreign");
  std::filesystem::remove_all(other_options.kv_options.db_path);
  kv::VectorDB other(other_options); MustOK(other.Open());
  const auto* foreign = other.GetSnapshot();
  CHECK(!db.Search({1,0,0}, 2, &results, kv::ReadOptions{foreign}).ok());
  other.ReleaseSnapshot(foreign); MustOK(other.Close());
  MustOK(db.Close()); MustOK(db.Close());
  CHECK(!db.Search({1,0,0}, 1, &results).ok());
  CHECK(!db.PutVector("closed", {1,0,0}).ok());
  MustOK(db.Open());
  CHECK(!db.Search({1,0,0}, 2, &results, kv::ReadOptions{fresh}).ok());
  db.ReleaseSnapshot(fresh); MustOK(db.Search({1,0,0}, 2, &results)); CHECK(results.size() == 1);
  MustOK(db.Close());
  std::filesystem::remove_all(options.kv_options.db_path);
  std::filesystem::remove_all(other_options.kv_options.db_path);
}

void TestLongUpdatesReclaimGraphIndependently() {
  auto options = Options("extension_reclaim"); options.index_rebuild_operations = 16;
  std::filesystem::remove_all(options.kv_options.db_path);
  kv::VectorDB db(options); MustOK(db.Open());
  MustOK(db.PutVector("old", {1,0,0})); const auto* snapshot = db.GetSnapshot();
  for (int i = 0; i < 256; ++i) {
    kv::VectorWriteBatch batch; MustOK(batch.PutVector("live", {float(i),0,1}));
    batch.Delete("old"); batch.Put("plain:" + std::to_string(i), "value");
    MustOK(db.Write(batch));
    CHECK(db.VectorIndexStats().node_count <= 17);
  }
  MustOK(db.Compact()); MustOK(db.RebuildIndex()); CHECK(db.VectorIndexStats().node_count == 1);
  std::vector<kv::VectorResult> results;
  MustOK(db.Search({1,0,0}, 1, &results, kv::ReadOptions{snapshot})); CHECK(results[0].key == "old");
  db.ReleaseSnapshot(snapshot); MustOK(db.Close()); std::filesystem::remove_all(options.kv_options.db_path);
}

void TestConcurrentBatchViewsRebuildAndClose() {
  auto options = Options("extension_concurrent_views"); options.index_rebuild_operations = 32;
  std::filesystem::remove_all(options.kv_options.db_path);
  kv::VectorDB db(options); MustOK(db.Open());
  auto write_pair = [&](int version) {
    kv::VectorWriteBatch batch;
    MustOK(batch.PutVector("a", {1,0,0}, std::to_string(version)));
    MustOK(batch.PutVector("b", {0,1,0}, std::to_string(version)));
    return db.Write(batch);
  };
  MustOK(write_pair(0)); std::atomic<bool> failed{false};
  std::thread writer([&] { for (int i = 1; i <= 100; ++i) if (!write_pair(i).ok()) failed = true; });
  std::thread rebuilder([&] { for (int i = 0; i < 20; ++i) if (!db.RebuildIndex().ok()) failed = true; });
  std::thread reader([&] {
    for (int i = 0; i < 100; ++i) {
      std::vector<std::vector<kv::VectorResult>> results;
      if (!db.SearchBatch({{1,0,0},{0,1,0}}, 2, 32, &results).ok() || results.size() != 2) { failed = true; continue; }
      if (results[0].size() != 2 || results[1].size() != 2) { failed = true; continue; }
      const auto version = results[0][0].metadata;
      for (const auto& query : results) for (const auto& result : query) if (result.metadata != version) failed = true;
    }
  });
  writer.join(); rebuilder.join(); reader.join(); CHECK(!failed);
  std::atomic<bool> started{false};
  std::thread searcher([&] {
    started = true;
    for (int i = 0; i < 100; ++i) {
      std::vector<kv::VectorResult> results;
      auto status = db.Search({1,0,0}, 2, &results);
      if (status.ok() && results.size() != 2) failed = true;
      if (!status.ok() && !results.empty()) failed = true;
    }
  });
  while (!started) std::this_thread::yield();
  MustOK(db.Close()); searcher.join(); CHECK(!failed);
  MustOK(db.Open()); MustOK(db.Close()); std::filesystem::remove_all(options.kv_options.db_path);
}

void TestIteratorFailureDoesNotPublishPartialIndex() {
  auto options = Options("extension_iterator_failure");
  options.kv_options.memtable_entries_limit = 1000;
  options.kv_options.block_size = 64;
  options.kv_options.block_cache_capacity = 0;
  std::filesystem::remove_all(options.kv_options.db_path);
  kv::VectorDB db(options); MustOK(db.Open());
  for (int i = 0; i < 20; ++i) MustOK(db.PutVector(Key(i), {float(i),0,1}));
  MustOK(db.Close()); MustOK(db.Open());
  const auto* snapshot = db.GetSnapshot();
  const auto files = ListSSTables(options.kv_options.db_path); CHECK(files.size() == 1);
  std::ifstream input(files[0], std::ios::binary);
  const std::string original{std::istreambuf_iterator<char>(input), {}};
  const auto index_offset = kv::DecodeFixed64(original.data() + original.size() - 40);
  CHECK(index_offset > 64);
  std::string damaged = original;
  // Last data-block checksum: earlier blocks are scanned successfully first.
  damaged[index_offset - 1] ^= 0x40;
  auto replace = [&](const std::string& bytes) {
    std::ofstream output(files[0], std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
  };
  replace(damaged);
  std::vector<kv::VectorResult> results{{"partial",0,{}}};
  CHECK(db.Search({1,0,1}, 10, &results, kv::ReadOptions{snapshot}).IsCorruption());
  CHECK(results.empty());
  CHECK(db.BruteForceSearch({1,0,1}, 10, &results).IsCorruption()); CHECK(results.empty());
  CHECK(db.RebuildIndex().IsCorruption()); CHECK(db.GetIndexState() == kv::IndexState::Dirty);
  CHECK(!db.Search({1,0,1}, 10, &results).ok()); CHECK(results.empty());
  replace(original); MustOK(db.RebuildIndex());
  CHECK(db.VectorIndexStats().node_count == 20);
  db.ReleaseSnapshot(snapshot); MustOK(db.Close());
  std::filesystem::remove_all(options.kv_options.db_path);
}

int CrashChild(const char* path) {
  auto options = Options("unused"); options.kv_options.db_path = path;
  options.testing_after_kv_commit = [] { _exit(73); };
  kv::VectorDB db(options); if (!db.Open().ok()) return 1;
  kv::VectorWriteBatch batch; MustOK(batch.PutVector("durable-a", {1,0,0}));
  MustOK(batch.PutVector("durable-b", {0,1,0}));
  (void)db.Write(batch); return 2;
}

void TestExitAfterKVCommit(const char* executable) {
  auto options = Options("extension_process_exit");
  std::filesystem::remove_all(options.kv_options.db_path);
  auto binary = std::filesystem::absolute(executable).string();
  auto path = options.kv_options.db_path.string();
  char* arguments[] = {binary.data(), const_cast<char*>("--crash-after-kv"), path.data(), nullptr};
  pid_t child = 0; CHECK(posix_spawn(&child, binary.c_str(), nullptr, nullptr, arguments, environ) == 0);
  int status = 0; CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status)); CHECK(WEXITSTATUS(status) == 73);
  kv::VectorDB db(options); MustOK(db.Open());
  std::vector<kv::VectorResult> results; MustOK(db.Search({1,0,0}, 10, &results)); CHECK(results.size() == 2);
  CHECK(results[0].key == "durable-a");
  MustOK(db.Close()); std::filesystem::remove_all(options.kv_options.db_path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--crash-after-kv") return CrashChild(argv[2]);
  TestAtomicMixedBatchesAndValidation();
  TestCommittedIndexFailureAndRebuild();
  TestUnknownKVOutcome();
  TestLifecycleAndSnapshotOwnership();
  TestLongUpdatesReclaimGraphIndependently();
  TestConcurrentBatchViewsRebuildAndClose();
  TestIteratorFailureDoesNotPublishPartialIndex();
  TestExitAfterKVCommit(argv[0]);
  std::cout << "vector_db_failure_test passed\n";
}
