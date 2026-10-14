#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "vector_db.h"
#include "format.h"
#include "test_support.h"

namespace {

void MustOK(const kv::VectorWriteResult& result) {
  if (!result.ok()) { std::cerr << result.ToString() << "\n"; std::abort(); }
}

void TestVectorWriteBatchWALAndDBLifecycle() {
  const std::filesystem::path db_path = TestDBPath("test_db_vector_lifecycle");
  std::filesystem::remove_all(db_path);
  std::filesystem::create_directories(db_path);
  kv::Manifest initial_manifest(db_path);
  MustOK(initial_manifest.Save(kv::VersionEdit{2, 1, 0, {}}));

  kv::VectorWriteBatch vector_batch;
  MustOK(vector_batch.PutVector("vector:a", {1.0f, 0.0f, 0.0f},
                                "{\"group\":\"a\"}"));
  MustOK(vector_batch.PutVector("vector:b", {0.0f, 1.0f, 0.0f},
                                "{\"group\":\"b\"}"));
  vector_batch.Put("plain", "ordinary-value");
  CHECK(vector_batch.Count() == 3);

  kv::WriteBatch decoded_batch;
  MustOK(decoded_batch.Decode(vector_batch.Encode()));
  CHECK(decoded_batch.Count() == 3);
  CHECK(decoded_batch.Operations()[0].type == kv::WriteBatchOpType::kPut);

  {
    kv::WALWriter wal(db_path / "wal_000001.log");
    MustOK(wal.Open());
    MustOK(wal.AppendBatch(decoded_batch, 1));
    wal.Close();
  }

  kv::VectorOptions options;
  options.kv_options.db_path = db_path;
  options.vector_dimension = 3;
  options.kv_options.memtable_entries_limit = 2;
  options.kv_options.level0_sstable_limit = 100;

  {
    kv::VectorOptions wrong_dimension = options;
    wrong_dimension.vector_dimension = 2;
    kv::VectorDB db(wrong_dimension);
    CHECK(!db.Open().ok());
  }

  {
    kv::VectorDB db(options);
    MustOK(db.Open());

    kv::VectorRecord record;
    MustOK(db.GetVector("vector:a", &record));
    CHECK(record.key == "vector:a");
    CHECK(record.vector == std::vector<float>({1.0f, 0.0f, 0.0f}));
    CHECK(record.metadata == "{\"group\":\"a\"}");
    CHECK(!db.GetVector("plain", &record).ok());
    CHECK(!db.PutVector("wrong-dimension", {1.0f, 2.0f}).ok());

    const kv::Snapshot* snapshot = db.GetSnapshot();
    MustOK(db.PutVector("vector:a", {0.0f, 0.0f, 1.0f},
                        "{\"version\":2}"));

    std::vector<kv::VectorResult> results;
    MustOK(db.BruteForceSearch({1.0f, 0.0f, 0.0f}, 2, &results));
    CHECK(results.size() == 2);
    CHECK(results[0].key == "vector:a" || results[0].key == "vector:b");

    kv::ReadOptions snapshot_read;
    snapshot_read.snapshot = snapshot;
    MustOK(db.BruteForceSearch({1.0f, 0.0f, 0.0f}, 1, &results,
                               kv::VectorDistanceMetric::kL2,
                               snapshot_read));
    CHECK(results.size() == 1);
    CHECK(results[0].key == "vector:a");
    CHECK(std::fabs(results[0].distance) < 1e-6f);
    db.ReleaseSnapshot(snapshot);

    MustOK(db.Delete("vector:b"));
    MustOK(db.BruteForceSearch({0.0f, 1.0f, 0.0f}, 10, &results));
    CHECK(results.size() == 1);
    CHECK(results[0].key == "vector:a");
    MustOK(db.Compact());
    MustOK(db.Close());

    kv::Manifest manifest(db_path);
    MustOK(manifest.Load());
    CHECK(!manifest.SSTables().empty());
    for (const auto& meta : manifest.SSTables()) {
      CHECK(std::filesystem::exists(meta.file_path));
    }
  }

  {
    kv::VectorDB reopened(options);
    MustOK(reopened.Open());
    kv::VectorRecord record;
    MustOK(reopened.GetVector("vector:a", &record));
    CHECK(record.vector == std::vector<float>({0.0f, 0.0f, 1.0f}));
    CHECK(record.metadata == "{\"version\":2}");
    CHECK(reopened.GetVector("vector:b", &record).IsNotFound());

    std::vector<kv::VectorResult> results;
    MustOK(reopened.BruteForceSearch(
        {0.0f, 0.0f, 1.0f}, 10, &results,
        kv::VectorDistanceMetric::kCosine));
    CHECK(results.size() == 1);
    CHECK(results[0].key == "vector:a");
    CHECK(std::fabs(results[0].distance) < 1e-6f);
    MustOK(reopened.Close());
  }

  std::filesystem::remove_all(db_path);
}

void TestVectorBruteForceSearchAcceptance() {
  const std::filesystem::path db_path = TestDBPath("test_db_vector_acceptance");
  std::filesystem::remove_all(db_path);

  kv::VectorOptions options;
  options.kv_options.db_path = db_path;
  options.vector_dimension = 768;
  options.kv_options.memtable_entries_limit = 2000;
  options.kv_options.level0_sstable_limit = 100;

  {
    kv::VectorDB db(options);
    MustOK(db.Open());
    kv::VectorWriteBatch batch;
    for (size_t i = 0; i < 1000; ++i) {
      std::vector<float> vector(768, 0.0f);
      vector[0] = static_cast<float>(i);
      MustOK(batch.PutVector("vector:" + std::to_string(i), vector,
                             "{\"id\":" + std::to_string(i) + "}"));
    }
    MustOK(db.Write(batch));
    MustOK(db.Put("plain", "not-a-vector"));

    std::vector<float> query(768, 0.0f);
    query[0] = 500.25f;
    std::vector<kv::VectorResult> results;
    MustOK(db.BruteForceSearch(query, 10, &results));
    const std::vector<std::string> expected = {
        "vector:500", "vector:501", "vector:499", "vector:502",
        "vector:498", "vector:503", "vector:497", "vector:504",
        "vector:496", "vector:505"};
    CHECK(results.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      CHECK(results[i].key == expected[i]);
      if (i > 0) {
        CHECK(results[i - 1].distance <= results[i].distance);
      }
    }
    MustOK(db.Close());

    const auto index_files = ListHNSWIndexes(db_path);
    CHECK(index_files.empty());
  }

  {
    kv::VectorDB reopened(options);
    MustOK(reopened.Open());
    std::vector<float> query(768, 0.0f);
    query[0] = 500.25f;
    std::vector<kv::VectorResult> results;
    MustOK(reopened.BruteForceSearch(query, 10, &results));
    CHECK(results.size() == 10);
    CHECK(results.front().key == "vector:500");
    MustOK(reopened.Close());
  }

  std::filesystem::remove_all(db_path);
}

void TestHNSWLSMIntegrationAndMVCC() {
  const std::filesystem::path db_path = TestDBPath("test_db_hnsw_lsm");
  std::filesystem::remove_all(db_path);

  std::mutex compaction_test_mu;
  std::condition_variable compaction_test_cv;
  bool compaction_ready_to_publish = false;
  bool allow_compaction_publish = false;
  bool search_completed_during_build = false;

  kv::VectorOptions options;
  options.kv_options.db_path = db_path;
  options.vector_dimension = 3;
  options.hnsw_max_neighbors = 8;
  options.hnsw_ef_construction = 64;
  options.kv_options.memtable_entries_limit = 3;
  options.kv_options.level0_sstable_limit = 100;
  options.kv_options.testing_before_compaction_publish = [&] {
    std::unique_lock<std::mutex> lock(compaction_test_mu);
    compaction_ready_to_publish = true;
    compaction_test_cv.notify_all();
    compaction_test_cv.wait(lock, [&] { return allow_compaction_publish; });
  };

  {
    kv::VectorDB db(options);
    MustOK(db.Open());
    MustOK(db.PutVector("a", {1.0f, 0.0f, 0.0f}, "{\"v\":1}"));
    MustOK(db.PutVector("b", {0.0f, 1.0f, 0.0f}, "{\"v\":1}"));
    MustOK(db.PutVector("c", {0.0f, 0.0f, 1.0f}, "{\"v\":1}"));

    std::vector<kv::VectorResult> results;
    MustOK(db.Search({1.0f, 0.0f, 0.0f}, 2, 32, &results,
                     kv::ReadOptions{}));
    CHECK(results.size() == 2);
    CHECK(results[0].key == "a");

    std::vector<std::vector<kv::VectorResult>> batch_results;
    MustOK(db.SearchBatch({{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, 2,
                          32, &batch_results));
    CHECK(batch_results.size() == 2);
    CHECK(batch_results[0].size() == 2);
    CHECK(batch_results[0][0].key == "a");
    CHECK(batch_results[1].size() == 2);
    CHECK(batch_results[1][0].key == "b");

    const kv::Snapshot* snapshot = db.GetSnapshot();
    MustOK(db.PutVector("a", {0.0f, 0.0f, 2.0f}, "{\"v\":2}"));
    MustOK(db.Delete("b"));

    MustOK(db.Search({1.0f, 0.0f, 0.0f}, 3, 32, &results));
    CHECK(!results.empty());
    CHECK(results[0].key == "c" || results[0].key == "a");
    for (const auto& result : results) {
      CHECK(result.key != "b");
    }

    kv::ReadOptions snapshot_read;
    snapshot_read.snapshot = snapshot;
    MustOK(db.Search({1.0f, 0.0f, 0.0f}, 2, 32, &results,
                     snapshot_read));
    CHECK(results.size() == 2);
    CHECK(results[0].key == "a");
    CHECK(results[0].metadata == "{\"v\":1}");
    bool found_b = false;
    for (const auto& result : results) {
      found_b = found_b || result.key == "b";
    }
    CHECK(found_b);
    MustOK(db.SearchBatch({{1.0f, 0.0f, 0.0f}}, 2, 32, &batch_results,
                          snapshot_read));
    CHECK(batch_results.size() == 1);
    CHECK(batch_results[0].size() == 2);
    CHECK(batch_results[0][0].key == "a");
    db.ReleaseSnapshot(snapshot);

    CHECK(WaitForSSTableCount(&db, 1));
    CHECK(ListHNSWIndexes(db_path).empty());
    // A historical side file belongs to the extension, even during KV maintenance.
    std::ofstream(db_path / "index_999999.hnsw") << "legacy-side-file";
    const auto old_indexes = ListHNSWIndexes(db_path);
    CHECK(old_indexes.size() == 1);

    kv::Status compaction_status;
    std::thread compactor([&] { compaction_status = db.Compact(); });
    {
      std::unique_lock<std::mutex> lock(compaction_test_mu);
      CHECK(compaction_test_cv.wait_for(
          lock, std::chrono::seconds(2),
          [&] { return compaction_ready_to_publish; }));
    }

    MustOK(db.PutVector("during-compaction", {9.0f, 9.0f, 9.0f},
                        "{\"v\":3}"));
    kv::Status concurrent_search_status;
    std::vector<kv::VectorResult> concurrent_results;
    std::thread reader([&] {
      concurrent_search_status =
          db.Search({9.0f, 9.0f, 9.0f}, 1, 32, &concurrent_results);
      {
        std::lock_guard<std::mutex> lock(compaction_test_mu);
        search_completed_during_build = true;
      }
      compaction_test_cv.notify_all();
    });

    bool search_was_nonblocking = false;
    {
      std::unique_lock<std::mutex> lock(compaction_test_mu);
      search_was_nonblocking = compaction_test_cv.wait_for(
          lock, std::chrono::seconds(2),
          [&] { return search_completed_during_build; });
      allow_compaction_publish = true;
    }
    compaction_test_cv.notify_all();
    reader.join();
    compactor.join();
    CHECK(search_was_nonblocking);
    MustOK(concurrent_search_status);
    CHECK(concurrent_results.size() == 1);
    CHECK(concurrent_results[0].key == "during-compaction");
    MustOK(compaction_status);

    MustOK(db.Search({9.0f, 9.0f, 9.0f}, 1, 32, &concurrent_results));
    CHECK(concurrent_results.size() == 1);
    CHECK(concurrent_results[0].key == "during-compaction");

    const auto new_indexes = ListHNSWIndexes(db_path);
    CHECK(new_indexes == old_indexes);
    for (const auto& old_index : old_indexes) {
      CHECK(std::filesystem::exists(old_index));
    }

    MustOK(db.PutVector("after-compaction", {1.0f, 0.0f, 0.0f},
                        "{\"v\":3}"));
    MustOK(db.Search({1.0f, 0.0f, 0.0f}, 10, 32, &results));
    bool found_after_compaction = false;
    for (const auto& result : results) {
      found_after_compaction =
          found_after_compaction || result.key == "after-compaction";
    }
    CHECK(found_after_compaction);
    MustOK(db.Close());
  }

  {
    kv::VectorDB db(options);
    MustOK(db.Open());
    std::vector<kv::VectorResult> results;
    MustOK(db.Search({1.0f, 0.0f, 0.0f}, 3, 32, &results));
    CHECK(!results.empty());
    CHECK(results[0].key == "after-compaction");
    bool found_c = false;
    for (const auto& result : results) {
      CHECK(result.key != "b");
      found_c = found_c || result.key == "c";
    }
    CHECK(found_c);
  }

  std::filesystem::remove_all(db_path);
}

void TestHNSWCompactionRecallAndVersionCleanup() {
  const std::filesystem::path db_path =
      TestDBPath("test_db_hnsw_compaction_recall");
  std::filesystem::remove_all(db_path);

  constexpr size_t kDimension = 8;
  auto make_vector = [](size_t id, size_t generation) {
    std::vector<float> vector(kDimension);
    for (size_t dimension = 0; dimension < kDimension; ++dimension) {
      vector[dimension] = static_cast<float>(
          std::sin((id + 1) * (dimension + 1) * 0.17 + generation * 0.11));
    }
    return vector;
  };

  kv::VectorOptions options;
  options.kv_options.db_path = db_path;
  options.vector_dimension = kDimension;
  options.hnsw_max_neighbors = 16;
  options.hnsw_ef_construction = 128;
  options.kv_options.memtable_entries_limit = 50;
  options.kv_options.level0_sstable_limit = 100;

  kv::VectorDB db(options);
  MustOK(db.Open());
  std::vector<std::vector<float>> live_vectors(160);
  for (size_t id = 0; id < 100; ++id) {
    live_vectors[id] = make_vector(id, 0);
    MustOK(db.PutVector("recall:" + std::to_string(id), live_vectors[id]));
  }
  for (size_t id = 0; id < 20; ++id) {
    live_vectors[id] = make_vector(id, 1);
    MustOK(db.PutVector("recall:" + std::to_string(id), live_vectors[id]));
  }
  for (size_t id = 20; id < 40; ++id) {
    MustOK(db.Delete("recall:" + std::to_string(id)));
  }
  for (size_t id = 100; id < 160; ++id) {
    live_vectors[id] = make_vector(id, 0);
    MustOK(db.PutVector("recall:" + std::to_string(id), live_vectors[id]));
  }
  CHECK(WaitForSSTableCount(&db, 4));

  const std::vector<size_t> query_ids = {0, 7, 19, 40, 75, 100, 127, 159};
  auto measure_recall = [&] {
    size_t hits = 0;
    size_t expected = 0;
    for (size_t query_id : query_ids) {
      std::vector<kv::VectorResult> exact;
      std::vector<kv::VectorResult> approximate;
      MustOK(db.BruteForceSearch(live_vectors[query_id], 10, &exact));
      MustOK(db.Search(live_vectors[query_id], 10, 256, &approximate));
      expected += exact.size();
      for (const auto& result : exact) {
        hits += std::any_of(
            approximate.begin(), approximate.end(),
            [&](const kv::VectorResult& candidate) {
              return candidate.key == result.key;
            });
      }
    }
    return expected == 0
               ? 1.0
               : static_cast<double>(hits) / static_cast<double>(expected);
  };

  const double recall_before = measure_recall();
  MustOK(db.Compact());
  const double recall_after = measure_recall();
  CHECK(db.VectorIndexStats().node_count == 180);
  CHECK(recall_after >= 0.95);
  CHECK(recall_after + 1e-12 >= recall_before);

  CHECK(ListHNSWIndexes(db_path).empty());
  MustOK(db.RebuildIndex());
  CHECK(db.VectorIndexStats().node_count == 140);
  CHECK(measure_recall() >= 0.95);

  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestConcurrentHNSWSearchAndWrites() {
  const std::filesystem::path db_path = TestDBPath("test_db_hnsw_concurrent");
  std::filesystem::remove_all(db_path);

  kv::VectorOptions options;
  options.kv_options.db_path = db_path;
  options.vector_dimension = 8;
  options.kv_options.memtable_entries_limit = 64;
  options.kv_options.level0_sstable_limit = 100;
  kv::VectorDB db(options);
  MustOK(db.Open());

  std::atomic<bool> failed{false};
  std::vector<std::thread> writers;
  for (int thread_id = 0; thread_id < 4; ++thread_id) {
    writers.emplace_back([&, thread_id] {
      for (int i = 0; i < 100; ++i) {
        std::vector<float> vector(8, 0.0f);
        vector[static_cast<size_t>(thread_id)] = 1.0f;
        if (!db.PutVector("w" + std::to_string(thread_id) + ":" +
                              std::to_string(i),
                          vector)
                 .ok()) {
          failed.store(true);
          return;
        }
      }
    });
  }

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      for (int round = 0; round < 100; ++round) {
        std::vector<kv::VectorResult> results;
        if (!db.Search({1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                       10, 64, &results)
                 .ok()) {
          failed.store(true);
          return;
        }
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }
  for (auto& reader : readers) {
    reader.join();
  }
  CHECK(!failed.load());
  std::vector<kv::VectorResult> results;
  MustOK(db.Search({1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                   10, 128, &results));
  CHECK(results.size() == 10);
  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestVectorCompactionByteBudget() {
  for (const bool small_budget : {false, true}) {
    const auto path = TestDBPath(small_budget ? "vector_small_l1" : "vector_default_l1");
    std::filesystem::remove_all(path);
    kv::VectorOptions options;
    options.kv_options.db_path = path;
    options.vector_dimension = 768;
    options.kv_options.memtable_entries_limit = 2;
    options.kv_options.level0_sstable_limit = 1;
    if (small_budget) {
      options.kv_options.level1_size_limit_bytes = 1024;
    }
    kv::VectorDB db(options);
    MustOK(db.Open());
    for (size_t i = 0; i < 4; ++i) {
      MustOK(db.PutVector(Key(i), std::vector<float>(768, static_cast<float>(i))));
    }
    CHECK(db.VectorIndexStats().node_count == 4);
    // Close joins the worker after it drains flushes and automatic compactions.
    MustOK(db.Close());
    const auto stats = db.Stats();
    CHECK(stats.flush_count == 2);
    CHECK(db.GetIndexState() == kv::IndexState::Closed);
    CHECK(stats.level0_sstable_count == 0);
    CHECK(stats.level1_sstable_count == (small_budget ? 0 : 1));
    CHECK(stats.level2_sstable_count == (small_budget ? 1 : 0));
    CHECK(stats.compaction_count == (small_budget ? 2 : 1));
    MustOK(db.Open());
    kv::VectorRecord record;
    MustOK(db.GetVector(Key(3), &record));
    CHECK(record.vector == std::vector<float>(768, 3.0f));
    MustOK(db.Close());
    std::filesystem::remove_all(path);
  }
}

}  // namespace

int main() {
  TestVectorWriteBatchWALAndDBLifecycle();
  TestVectorBruteForceSearchAcceptance();
  TestHNSWLSMIntegrationAndMVCC();
  TestHNSWCompactionRecallAndVersionCleanup();
  TestConcurrentHNSWSearchAndWrites();
  TestVectorCompactionByteBudget();
  std::cout << "vector_db_test passed\n";
}
