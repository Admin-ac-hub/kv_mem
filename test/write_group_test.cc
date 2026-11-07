#include <limits>

#include "db.h"
#include "write_group_support.h"

namespace {
void TestBatchHelpers() {
  kv::WriteBatch batch;
  batch.Put("x", "a");
  batch.Delete("y");
  CHECK(batch.EncodedSize() == batch.Encode().size());
  batch.Append(batch);
  CHECK(batch.Count() == 4 && batch.EncodedSize() == batch.Encode().size());
  kv::WriteBatch copy;
  copy.Reserve(4);
  copy.Append(batch);
  CHECK(copy.Encode() == batch.Encode());
  copy.Clear();
  CHECK(copy.EncodedSize() == 0);
}

void TestGroups(bool grouping, size_t limit, bool fail, bool concurrent_close,
                bool throw_sync = false) {
  const auto path = TestDBPath("write_groups");
  std::filesystem::remove_all(path);
  WriteGate gate;
  gate.fail_second = fail;
  kv::Options options;
  options.db_path = path;
  options.memtable_entries_limit = 1000;
  options.enable_write_grouping = grouping;
  options.write_group_max_bytes = limit;
  options.testing_after_write_enqueue = [&] { gate.Enqueued(); };
  options.testing_before_wal_sync = [&] {
    auto status = gate.Sync();
    if (throw_sync && !status.ok())
      throw std::runtime_error("group WAL exception");
    return status;
  };
  kv::DB db(options);
  MustOK(db.Open());
  const auto* before = db.GetSnapshot();
  auto before_iterator = db.NewIterator();
  std::atomic<int> completed{0};
  std::vector<kv::Status> results(4);
  std::vector<std::thread> writers;
  writers.emplace_back([&] {
    results[0] = db.Put("x", "old");
    ++completed;
  });
  gate.WaitSync(1);
  kv::WriteBatch a, b, c;
  a.Put("x", "A");
  a.Put("p", "1");
  a.Put("q", "1");
  b.Delete("x");
  b.Put("x", "B");
  c.Put("x", "C");
  c.Delete("missing");
  const kv::WriteBatch* batches[] = {&a, &b, &c};
  for (int i = 0; i < 3; ++i) {
    writers.emplace_back([&, i] {
      results[i + 1] = db.Write(*batches[i]);
      ++completed;
    });
    gate.WaitEnqueued(i + 2);
  }
  kv::WriteBatch bad;
  bad.Put("valid", "v");
  bad.Put("", "bad");
  CHECK(db.Write(bad).IsInvalidArgument());
  CHECK(completed == 0);
  kv::Status close_status;
  std::thread closer;
  if (concurrent_close)
    closer = std::thread([&] { close_status = db.Close(); });
  gate.Release(1);
  gate.WaitSync(2);
  CHECK(completed <= 1);  // No follower may acknowledge an unsynced group.
  gate.Release(2);
  for (auto& writer : writers) writer.join();
  if (closer.joinable()) {
    closer.join();
    MustOK(close_status);
  }
  MustOK(results[0]);
  const bool merged = grouping && limit > 100;
  size_t expected_syncs = 1, expected_max = 1, packed_bytes = 0, packed_ops = 0;
  for (const auto* batch : batches) {
    if (packed_ops != 0 && (!grouping || packed_bytes > limit ||
                            batch->EncodedSize() > limit - packed_bytes)) {
      ++expected_syncs;
      expected_max = std::max(expected_max, packed_ops);
      packed_bytes = 0;
      packed_ops = 0;
    }
    packed_bytes += batch->EncodedSize();
    packed_ops += batch->Count();
  }
  ++expected_syncs;
  expected_max = std::max(expected_max, packed_ops);
  if (fail) {
    for (int i = 1; i < 4; ++i) CHECK(results[i].IsIOError());
    CHECK(db.Write(kv::WriteBatch{}).IsIOError());
    CHECK(db.Put("blocked", "v").IsIOError());
    CHECK(db.Close().IsIOError());
    MustOK(db.Open());
  } else {
    for (const auto& result : results) MustOK(result);
    if (concurrent_close) MustOK(db.Open());
  }
  std::string value;
  MustOK(db.Get("x", &value));
  CHECK(value == (fail && !merged ? "A" : "C"));
  CHECK(db.Get("valid", &value).IsNotFound());
  CHECK(db.Get("blocked", &value).IsNotFound());
  if (!fail && !concurrent_close) {
    CHECK(db.Get("x", &value, kv::ReadOptions{before}).IsNotFound());
    before_iterator->SeekToFirst();
    CHECK(!before_iterator->Valid());
    MustOK(before_iterator->status());
    auto after_iterator = db.NewIterator();
    after_iterator->SeekToFirst();
    CHECK(after_iterator->Valid() && after_iterator->key() == "p");
    after_iterator->Next();
    CHECK(after_iterator->Valid() && after_iterator->key() == "q");
    after_iterator->Next();
    CHECK(after_iterator->Valid() && after_iterator->key() == "x" &&
          after_iterator->value() == "C");
    after_iterator->Next();
    CHECK(!after_iterator->Valid());
    MustOK(after_iterator->status());
    CHECK(db.GetSnapshot()->sequence() == 8);
    kv::WALReader reader(path / "wal_000001.log");
    kv::WALBatchRecord record;
    bool eof = false;
    size_t records = 0, operations = 0;
    do {
      MustOK(reader.ReadNext(&record, &eof));
      if (!eof) {
        CHECK(record.sequence == operations + 1);
        operations += record.batch.Count();
        ++records;
      }
    } while (!eof);
    CHECK(records == expected_syncs);
    CHECK(operations == 8);
  }
  const auto stats = db.Stats();
  CHECK(stats.wal_sync_count == (fail ? 1 : expected_syncs));
  CHECK(stats.wal_committed_operations == (fail ? 1 : 8));
  CHECK(stats.max_wal_batch_operations == (fail ? 1 : expected_max));
  MustOK(db.Write(kv::WriteBatch{}));
  CHECK(db.Stats().wal_sync_count == stats.wal_sync_count);
  MustOK(db.Close());
  MustOK(db.Open());
  CHECK(db.Stats().wal_sync_count == stats.wal_sync_count);
  MustOK(db.Put("after-reopen", "v"));
  CHECK(db.Stats().wal_sync_count == stats.wal_sync_count + 1);
  MustOK(db.Close());
  std::filesystem::remove_all(path);
}

void TestEnqueueExceptionsAndEmptyHealth() {
  const auto path = TestDBPath("write_group_enqueue_exception");
  std::filesystem::remove_all(path);
  int failure = 0;
  kv::Options options;
  options.db_path = path;
  options.testing_after_write_enqueue = [&] {
    if (failure == 1) throw std::runtime_error("enqueue exception");
    if (failure == 2) throw 7;
  };
  kv::DB db(options);
  CHECK(db.Write(kv::WriteBatch{}).IsIOError());
  MustOK(db.Open());
  for (failure = 1; failure <= 2; ++failure)
    CHECK(db.Put("rejected", "v").IsIOError());
  failure = 0;
  MustOK(db.Write(kv::WriteBatch{}));
  CHECK(db.Stats().wal_sync_count == 0);
  MustOK(db.Put("accepted", "v"));
  std::string value;
  CHECK(db.Get("rejected", &value).IsNotFound());
  MustOK(db.Close());
  CHECK(db.Write(kv::WriteBatch{}).IsIOError());
  MustOK(db.Open());
  MustOK(db.Get("accepted", &value));
  MustOK(db.Close());
  std::filesystem::remove_all(path);
}

void TestSequenceBudget() {
  const auto path = TestDBPath("write_group_sequence");
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  kv::Manifest manifest(path);
  kv::VersionEdit edit;
  edit.last_sequence = std::numeric_limits<kv::SequenceNumber>::max() - 3;
  edit.next_file_number = 2;
  MustOK(manifest.Save(edit));
  WriteGate gate;
  kv::Options options;
  options.db_path = path;
  options.testing_after_write_enqueue = [&] { gate.Enqueued(); };
  options.testing_before_wal_sync = [&] { return gate.Sync(); };
  kv::DB db(options);
  MustOK(db.Open());
  std::vector<kv::Status> results(4);
  std::vector<std::thread> writers;
  writers.emplace_back([&] { results[0] = db.Put("0", "v"); });
  gate.WaitSync(1);
  for (int i = 1; i < 4; ++i) {
    writers.emplace_back(
        [&, i] { results[i] = db.Put(std::to_string(i), "v"); });
    gate.WaitEnqueued(i + 1);
  }
  gate.Release(1);
  gate.WaitSync(2);
  gate.Release(2);
  for (auto& writer : writers) writer.join();
  for (int i = 0; i < 3; ++i) MustOK(results[i]);
  CHECK(results[3].IsInvalidArgument());
  CHECK(db.Stats().wal_sync_count == 2);
  CHECK(db.GetSnapshot()->sequence() ==
        std::numeric_limits<kv::SequenceNumber>::max());
  MustOK(db.Close());
  MustOK(db.Open());
  std::string value;
  CHECK(db.Get("3", &value).IsNotFound());
  MustOK(db.Close());
  std::filesystem::remove_all(path);
}
}  // namespace

int main() {
  TestBatchHelpers();
  TestGroups(true, 1024 * 1024, false, false);
  TestGroups(false, 1024 * 1024, false, false);
  TestGroups(true, 54, false, false);  // Two batches exactly fill the byte cap.
  TestGroups(true, 1, false,
             false);  // Oversized original batches remain intact.
  TestGroups(true, 1024 * 1024, true, false);
  TestGroups(true, 1024 * 1024, true, false, true);
  TestGroups(true, 1024 * 1024, false, true);
  TestGroups(false, 1024 * 1024, false, true);
  TestSequenceBudget();
  TestEnqueueExceptionsAndEmptyHealth();
  kv::Options invalid;
  invalid.db_path = TestDBPath("write_group_invalid");
  invalid.write_group_max_bytes = 0;
  kv::DB db(invalid);
  CHECK(db.Open().IsInvalidArgument());
  std::cout << "write_group_test passed\n";
}
