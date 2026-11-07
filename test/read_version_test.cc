#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>

#include "db.h"
#include "test_support.h"

namespace {

class Pause {
 public:
  void Arm(int skip = 0) {
    std::lock_guard<std::mutex> lock(mu_);
    remaining_ = skip + 1;
  }

  bool Block() {
    std::unique_lock<std::mutex> lock(mu_);
    if (remaining_ == 0 || --remaining_ != 0) return false;
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [&] { return released_; });
    return true;
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mu_);
    CHECK(cv_.wait_for(lock, std::chrono::seconds(10), [&] { return entered_; }));
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mu_);
    released_ = true;
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  int remaining_ = 0;
  bool entered_ = false;
  bool released_ = false;
};

void CheckOldView(kv::DB& db, const kv::ReadOptions& options = {}) {
  std::string value;
  for (const auto* key : {"a", "b", "gone"}) {
    MustOK(db.Get(key, &value, options));
    CHECK(value == "old");
  }
  CHECK(db.Get("fresh", &value, options).IsNotFound());
  auto iterator = db.NewIterator(options);
  iterator->SeekToFirst();
  for (const auto* key : {"a", "b", "gone"}) {
    CHECK(iterator->Valid() && iterator->key() == key);
    CHECK(iterator->value() == "old");
    iterator->Next();
  }
  CHECK(!iterator->Valid());
  MustOK(iterator->status());
}

void CheckNewView(kv::DB& db) {
  std::string value;
  for (const auto* key : {"a", "b", "fresh"}) {
    MustOK(db.Get(key, &value));
    CHECK(value == "new");
  }
  CHECK(db.Get("gone", &value).IsNotFound());
}

// During WAL fsync and after installation but before publication, every read
// API must finish against the old boundary. A failed commit must not advance it.
void TestBatchPublication(bool pause_before_publish, bool fail) {
  const auto path = TestDBPath("batch_publication_" +
      std::to_string(pause_before_publish) + "_" + std::to_string(fail));
  std::filesystem::remove_all(path);
  Pause pause;
  kv::Options options;
  options.db_path = path;
  if (pause_before_publish) {
    options.testing_before_sequence_publish = [&] {
      if (pause.Block() && fail) throw std::runtime_error("publication failure");
    };
  } else {
    options.testing_before_wal_sync = [&] {
      return pause.Block() && fail ? kv::Status::IOError("WAL sync failure")
                                   : kv::Status::OK();
    };
  }
  kv::DB db(options);
  MustOK(db.Open());
  kv::WriteBatch seed;
  for (const auto* key : {"a", "b", "gone"}) seed.Put(key, "old");
  MustOK(db.Write(seed));
  pause.Arm();
  kv::WriteBatch batch;
  batch.Put("a", "intermediate");
  batch.Put("a", "new");
  batch.Delete("gone");
  batch.Put("fresh", "new");
  batch.Put("b", "new");
  kv::Status write_status;
  std::thread writer([&] { write_status = db.Write(batch); });
  pause.Wait();

  auto read = std::async(std::launch::async, [&] {
    CheckOldView(db);
    const auto* snapshot = db.GetSnapshot();
    CHECK(snapshot != nullptr && snapshot->sequence() == 3);
    CheckOldView(db, {snapshot});
    return snapshot;
  });
  // This is a progress assertion: the writer stays paused until reads finish.
  CHECK(read.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  const auto* before = read.get();
  pause.Release();
  writer.join();
  CheckOldView(db, {before});
  const auto* after = db.GetSnapshot();
  CHECK(after->sequence() == (fail ? 3 : 8));
  db.ReleaseSnapshot(before);
  db.ReleaseSnapshot(after);
  if (fail) {
    CHECK(write_status.IsIOError());
    CheckOldView(db);
    CHECK(db.Put("rejected", "v").IsIOError());
    CHECK(db.Close().IsIOError());
  } else {
    MustOK(write_status);
    CheckNewView(db);
    MustOK(db.Close());
  }
  MustOK(db.Open());
  // A complete WAL record is replayed even if its sync/publication reported an
  // error. Recovery must publish the complete range and allocate after it.
  CheckNewView(db);
  const auto* recovered = db.GetSnapshot();
  CHECK(recovered->sequence() == 8);
  db.ReleaseSnapshot(recovered);
  MustOK(db.Put("recovered", "v"));
  const auto* next = db.GetSnapshot();
  CHECK(next->sequence() == 9);
  db.ReleaseSnapshot(next);
  MustOK(db.Close());
  std::filesystem::remove_all(path);
}

// Neither rotating the WAL, registering a flush nor publishing a compaction
// may hold up ordinary reads while MANIFEST synchronization is paused.
void TestReadsDuringManifestSync(int phase) {
  const auto path = TestDBPath("read_during_manifest_" + std::to_string(phase));
  std::filesystem::remove_all(path);
  Pause pause;
  kv::Options options;
  options.db_path = path;
  options.memtable_entries_limit = 1;
  options.level0_sstable_limit = 100;
  options.block_cache_capacity = 0;
  options.testing_before_manifest_sync = [&] {
    pause.Block();
    return kv::Status::OK();
  };
  kv::DB db(options);
  MustOK(db.Open());
  MustOK(db.Put("key", "old"));
  CHECK(WaitForSSTableCount(&db, 1));
  pause.Arm(phase == 1 ? 1 : 0);
  kv::Status status;
  const std::string expected = phase == 2 ? "old" : "new";
  std::thread maintenance([&] {
    status = phase == 2 ? db.Compact() : db.Put("key", "new");
  });
  pause.Wait();
  auto read = std::async(std::launch::async, [&] {
    std::string value;
    MustOK(db.Get("key", &value));
    CHECK(value == expected);
    auto iterator = db.NewIterator();
    iterator->SeekToFirst();
    CHECK(iterator->Valid() && iterator->key() == "key");
    CHECK(iterator->value() == expected);
    iterator->Next();
    CHECK(!iterator->Valid());
    MustOK(iterator->status());
  });
  CHECK(read.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  read.get();
  pause.Release();
  maintenance.join();
  MustOK(status);
  MustOK(db.Close());
  MustOK(db.Open());
  std::string value;
  MustOK(db.Get("key", &value));
  CHECK(value == expected);
  MustOK(db.Close());
  std::filesystem::remove_all(path);
}

void TestConcurrentBatchViews(size_t memtable_limit) {
  const auto path = TestDBPath("concurrent_batch_views_" + std::to_string(memtable_limit));
  std::filesystem::remove_all(path);
  kv::Options options;
  options.db_path = path;
  options.memtable_entries_limit = memtable_limit;
  options.level0_sstable_limit = 2;
  kv::DB db(options);
  MustOK(db.Open());
  kv::WriteBatch seed;
  seed.Put("a", "0");
  seed.Put("b", "0");
  MustOK(db.Write(seed));
  std::atomic<bool> start{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&] {
      while (!start.load()) std::this_thread::yield();
      for (int j = 0; j < 300; ++j) {
        auto iterator = db.NewIterator();
        iterator->SeekToFirst();
        CHECK(iterator->Valid() && iterator->key() == "a");
        const std::string expected = iterator->value();
        iterator->Next();
        CHECK(iterator->Valid() && iterator->key() == "b");
        CHECK(iterator->value() == expected);
        iterator->Next();
        CHECK(!iterator->Valid());
        MustOK(iterator->status());

        const auto* snapshot = db.GetSnapshot();
        CHECK(snapshot != nullptr && snapshot->sequence() % 2 == 0);
        std::string a, b;
        MustOK(db.Get("a", &a, {snapshot}));
        MustOK(db.Get("b", &b, {snapshot}));
        CHECK(a == b);
        db.ReleaseSnapshot(snapshot);
      }
    });
  }
  std::vector<std::thread> writers;
  for (int writer = 0; writer < 3; ++writer) {
    writers.emplace_back([&, writer] {
      while (!start.load()) std::this_thread::yield();
      for (int i = 1; i <= 100; ++i) {
        kv::WriteBatch batch;
        const auto value = std::to_string(writer * 100 + i);
        batch.Put("a", value);
        batch.Put("b", value);
        MustOK(db.Write(batch));
      }
    });
  }
  start.store(true);
  for (auto& writer : writers) writer.join();
  for (auto& reader : readers) reader.join();
  MustOK(db.Close());
  std::filesystem::remove_all(path);
}

}  // namespace

int main() {
  for (bool publish : {false, true}) {
    for (bool fail : {false, true}) TestBatchPublication(publish, fail);
  }
  for (int phase = 0; phase < 3; ++phase) TestReadsDuringManifestSync(phase);
  TestConcurrentBatchViews(6);
  TestConcurrentBatchViews(100000);  // Keep all concurrent inserts in the active table.
  std::cout << "read_version_test passed\n";
}
