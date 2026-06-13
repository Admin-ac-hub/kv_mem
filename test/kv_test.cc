#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "db.h"
#include "format.h"

namespace {

constexpr size_t kFlushLimit = 1024;

#ifndef KV_TEST_DB_ROOT
#define KV_TEST_DB_ROOT "test/test_dbs"
#endif

std::filesystem::path TestDBPath(const std::string& name) {
  std::filesystem::path path = std::filesystem::path(KV_TEST_DB_ROOT) / name;
  std::filesystem::create_directories(path.parent_path());
  return path;
}

void MustOK(const kv::Status& status) {
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    std::abort();
  }
}

std::string Key(size_t index) {
  return "k" + std::to_string(index);
}

void FillToFlush(kv::DB* db, const std::string& prefix, const std::string& value) {
  for (size_t i = 0; i < kFlushLimit; ++i) {
    MustOK(db->Put(prefix + Key(i), value + std::to_string(i)));
  }
}

bool WaitForSSTableCount(kv::DB* db, size_t count) {
  for (int i = 0; i < 200; ++i) {
    if (db->Stats().sstable_count >= count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForCompactionCount(kv::DB* db, std::uint64_t count) {
  for (int i = 0; i < 200; ++i) {
    if (db->Stats().compaction_count >= count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::vector<std::filesystem::path> ListSSTables(const std::filesystem::path& db_path) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
    std::uint64_t number = 0;
    if (entry.is_regular_file() && kv::ParseSSTableFileName(entry.path(), &number)) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

size_t CountWALFiles(const std::filesystem::path& db_path) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
    std::uint64_t number = 0;
    if (entry.is_regular_file() && kv::ParseWALFileName(entry.path(), &number)) {
      ++count;
    }
  }
  return count;
}

std::filesystem::path FirstSSTablePath(const std::filesystem::path& db_path) {
  auto files = ListSSTables(db_path);
  assert(!files.empty());
  return files.front();
}

size_t CountManifestRecords(const std::filesystem::path& db_path) {
  std::ifstream in(db_path / "MANIFEST", std::ios::binary);
  std::string line;
  size_t count = 0;
  while (std::getline(in, line)) {
    if (line.rfind("record ", 0) == 0) {
      ++count;
    }
  }
  return count;
}

void WriteManifest(const std::filesystem::path& db_path,
                   std::uint64_t next_file_number,
                   std::uint64_t wal_file_number) {
  std::ostringstream body;
  body << "version 1\n";
  body << "next_file_number " << next_file_number << '\n';
  body << "wal_file_number " << wal_file_number << '\n';
  body << "last_sequence 0\n";
  const std::string body_text = body.str();
  std::ofstream manifest(db_path / "MANIFEST", std::ios::trunc);
  manifest << body_text;
  manifest << "checksum " << kv::CRC32(body_text) << '\n';
}

void TestBasicWALRecovery() {
  const std::filesystem::path db_path = TestDBPath("test_db_basic");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Put("k1", "v1"));
    MustOK(db.Put("k2", "v2"));
    MustOK(db.Delete("k2"));

    std::string value;
    MustOK(db.Get("k1", &value));
    assert(value == "v1");
    assert(db.Get("k2", &value).IsNotFound());
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("k1", &value));
    assert(value == "v1");
    assert(db.Get("k2", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestFlushAndSSTableRecovery() {
  const std::filesystem::path db_path = TestDBPath("test_db_flush");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    FillToFlush(&db, "a", "v");

    assert(WaitForSSTableCount(&db, 1));
    assert(ListSSTables(db_path).size() == 1);
    assert(CountWALFiles(db_path) == 1);

    std::string value;
    MustOK(db.Get("a" + Key(0), &value));
    assert(value == "v0");
    MustOK(db.Get("a" + Key(1023), &value));
    assert(value == "v1023");

    MustOK(db.Put("wal_only", "still_in_wal"));
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("a" + Key(100), &value));
    assert(value == "v100");
    MustOK(db.Get("wal_only", &value));
    assert(value == "still_in_wal");
  }

  std::filesystem::remove_all(db_path);
}

void TestNewestSSTableWins() {
  const std::filesystem::path db_path = TestDBPath("test_db_versions");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Put("shared", "old"));
    FillToFlush(&db, "old", "v");
    assert(WaitForSSTableCount(&db, 1));
    MustOK(db.Put("shared", "new"));
    FillToFlush(&db, "new", "v");
    assert(WaitForSSTableCount(&db, 2));

    assert(ListSSTables(db_path).size() == 2);

    std::string value;
    MustOK(db.Get("shared", &value));
    assert(value == "new");
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("shared", &value));
    assert(value == "new");
  }

  std::filesystem::remove_all(db_path);
}

void TestDeleteFlushRecovery() {
  const std::filesystem::path db_path = TestDBPath("test_db_delete");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Put("gone", "value"));
    FillToFlush(&db, "first", "v");
    MustOK(db.Delete("gone"));
    FillToFlush(&db, "second", "v");

    std::string value;
    assert(db.Get("gone", &value).IsNotFound());
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    assert(db.Get("gone", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestInvalidInput() {
  const std::filesystem::path db_path = TestDBPath("test_db_invalid");
  std::filesystem::remove_all(db_path);

  kv::DB db(db_path);
  MustOK(db.Open());

  assert(!db.Put("bad\tkey", "value").ok());
  assert(!db.Put("bad\nkey", "value").ok());
  assert(!db.Delete("bad\tkey").ok());
  assert(!db.Put("key", "bad\tvalue").ok());
  assert(!db.Put("key", "bad\nvalue").ok());
  assert(!db.Put("key", "__DELETE__").ok());

  std::filesystem::remove_all(db_path);
}

void TestUpdateDeleteAndValues() {
  const std::filesystem::path db_path = TestDBPath("test_db_values");
  std::filesystem::remove_all(db_path);

  kv::DB db(db_path);
  MustOK(db.Open());
  MustOK(db.Put("k", "v1"));
  MustOK(db.Put("k", "v2"));
  MustOK(db.Delete("missing"));
  MustOK(db.Put("empty", ""));
  MustOK(db.Put("large", std::string(4096, 'x')));

  std::string value;
  MustOK(db.Get("k", &value));
  assert(value == "v2");
  assert(db.Get("missing", &value).IsNotFound());
  MustOK(db.Get("empty", &value));
  assert(value.empty());
  MustOK(db.Get("large", &value));
  assert(value.size() == 4096);

  std::filesystem::remove_all(db_path);
}

void TestWriteBatchOperationsAndRecovery() {
  const std::filesystem::path db_path = TestDBPath("test_db_write_batch");
  std::filesystem::remove_all(db_path);

  {
    kv::WriteBatch batch;
    assert(batch.Count() == 0);
    batch.Put("a", "v1");
    batch.Put("b", "v2");
    batch.Delete("missing");
    assert(batch.Count() == 3);

    kv::WriteBatch decoded;
    MustOK(decoded.Decode(batch.Encode()));
    assert(decoded.Count() == 3);

    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Write(decoded));

    kv::WriteBatch overwrite;
    overwrite.Put("a", "v3");
    overwrite.Delete("b");
    MustOK(db.Write(overwrite));

    std::string value;
    MustOK(db.Get("a", &value));
    assert(value == "v3");
    assert(db.Get("b", &value).IsNotFound());
    assert(db.Get("missing", &value).IsNotFound());

    overwrite.Clear();
    assert(overwrite.Count() == 0);
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("a", &value));
    assert(value == "v3");
    assert(db.Get("b", &value).IsNotFound());
    assert(db.Get("missing", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestTruncatedLastWALBatchIsIgnored() {
  const std::filesystem::path db_path = TestDBPath("test_db_truncated_batch");
  std::filesystem::remove_all(db_path);
  std::filesystem::create_directories(db_path);
  WriteManifest(db_path, 2, 1);

  {
    kv::WriteBatch durable;
    durable.Put("durable", "yes");
    kv::WriteBatch partial;
    partial.Put("partial_a", "no");
    partial.Delete("durable");

    kv::WALWriter wal(db_path / "wal_000001.log");
    MustOK(wal.Open());
    MustOK(wal.AppendBatch(durable, 1));
    MustOK(wal.AppendBatch(partial, 2));
    wal.Close();
  }

  const std::filesystem::path wal_path = db_path / "wal_000001.log";
  const auto original_size = std::filesystem::file_size(wal_path);
  std::filesystem::resize_file(wal_path, original_size - 3);

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("durable", &value));
    assert(value == "yes");
    assert(db.Get("partial_a", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestImmutableMemTableWriteAndReadOrdering() {
  const std::filesystem::path db_path = TestDBPath("test_db_immutable_order");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.memtable_entries_limit = 2;
  options.level0_sstable_limit = 100;

  {
    kv::DB db(options);
    MustOK(db.Open());
    MustOK(db.Put("shared", "old"));
    MustOK(db.Put("filler1", "v1"));
    MustOK(db.Put("shared", "new"));
    MustOK(db.Put("filler2", "v2"));
    MustOK(db.Put("active", "v3"));

    std::string value;
    MustOK(db.Get("shared", &value));
    assert(value == "new");
    MustOK(db.Get("filler1", &value));
    assert(value == "v1");
    MustOK(db.Get("active", &value));
    assert(value == "v3");

    assert(WaitForSSTableCount(&db, 2));
  }

  {
    kv::DB db(options);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("shared", &value));
    assert(value == "new");
    MustOK(db.Get("filler1", &value));
    assert(value == "v1");
    MustOK(db.Get("active", &value));
    assert(value == "v3");
  }

  std::filesystem::remove_all(db_path);
}

void TestCloseFlushesActiveAndImmutableMemTables() {
  const std::filesystem::path db_path = TestDBPath("test_db_close_flush");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.memtable_entries_limit = 3;
  options.level0_sstable_limit = 100;

  {
    kv::DB db(options);
    MustOK(db.Open());
    for (int i = 0; i < 8; ++i) {
      MustOK(db.Put("k" + std::to_string(i), "v" + std::to_string(i)));
    }
    MustOK(db.Close());
    assert(db.Stats().sstable_count >= 3);
  }

  {
    kv::DB db(options);
    MustOK(db.Open());
    for (int i = 0; i < 8; ++i) {
      std::string value;
      MustOK(db.Get("k" + std::to_string(i), &value));
      assert(value == "v" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(db_path);
}

void TestFlushFailureKeepsWALForRecovery() {
  const std::filesystem::path db_path = TestDBPath("test_db_flush_failure");
  std::filesystem::remove_all(db_path);

  kv::Options failing_options;
  failing_options.db_path = db_path;
  failing_options.memtable_entries_limit = 2;
  failing_options.level0_sstable_limit = 100;
  failing_options.testing_fail_flush = true;

  {
    kv::DB db(failing_options);
    MustOK(db.Open());
    MustOK(db.Put("survives_a", "va"));
    MustOK(db.Put("survives_b", "vb"));
    for (int i = 0; i < 200; ++i) {
      if (!db.Put("after_failure", "x").ok()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(CountWALFiles(db_path) >= 1);
    assert(!db.Close().ok());
  }

  kv::Options recovery_options;
  recovery_options.db_path = db_path;
  recovery_options.memtable_entries_limit = 2;
  recovery_options.level0_sstable_limit = 100;
  {
    kv::DB db(recovery_options);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("survives_a", &value));
    assert(value == "va");
    MustOK(db.Get("survives_b", &value));
    assert(value == "vb");
  }

  std::filesystem::remove_all(db_path);
}

void TestManifestRecoveryAndFallback() {
  const std::filesystem::path db_path = TestDBPath("test_db_manifest");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    FillToFlush(&db, "m", "v");
  }
  assert(std::filesystem::exists(db_path / "MANIFEST"));

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("m" + Key(1), &value));
    assert(value == "v1");
  }

  std::filesystem::remove(db_path / "MANIFEST");
  {
    kv::DB db(db_path);
    assert(db.Open().IsIOError());
  }

  std::filesystem::remove(db_path / "CURRENT");
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    assert(std::filesystem::exists(db_path / "MANIFEST"));
    std::string value;
    MustOK(db.Get("m" + Key(2), &value));
    assert(value == "v2");
  }

  {
    std::ofstream out(db_path / "MANIFEST", std::ios::trunc);
    out << "broken manifest\n";
  }
  {
    kv::DB db(db_path);
    assert(db.Open().IsCorruption());
  }

  std::filesystem::remove_all(db_path);
}

void TestBadWALReturnsCorruption() {
  const std::filesystem::path db_path = TestDBPath("test_db_bad_wal");
  std::filesystem::remove_all(db_path);
  std::filesystem::create_directories(db_path);
  WriteManifest(db_path, 1, 1);
  {
    std::ofstream wal(db_path / "wal_000001.log", std::ios::binary | std::ios::trunc);
    wal.put(static_cast<char>(99));
    wal << "bad";
  }

  kv::DB db(db_path);
  assert(db.Open().IsCorruption());
  std::filesystem::remove_all(db_path);
}

void TestManifestEditLogReplayAndTruncation() {
  const std::filesystem::path db_path = TestDBPath("test_db_manifest_edit_log");
  std::filesystem::remove_all(db_path);

  {
    kv::Options options;
    options.db_path = db_path;
    options.level0_sstable_limit = 100;
    kv::DB db(options);
    MustOK(db.Open());
    FillToFlush(&db, "e1", "v");
    assert(WaitForSSTableCount(&db, 1));
    FillToFlush(&db, "e2", "v");
    assert(WaitForSSTableCount(&db, 2));
    MustOK(db.Close());
  }

  assert(std::filesystem::exists(db_path / "CURRENT"));
  assert(CountManifestRecords(db_path) >= 3);
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("e1" + Key(10), &value));
    assert(value == "v10");
    MustOK(db.Get("e2" + Key(10), &value));
    assert(value == "v10");
  }

  const std::filesystem::path manifest_path = db_path / "MANIFEST";
  const auto original_size = std::filesystem::file_size(manifest_path);
  std::filesystem::resize_file(manifest_path, original_size - 5);
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("e1" + Key(20), &value));
    assert(value == "v20");
  }

  std::filesystem::remove_all(db_path);
}

void TestOrphanSSTableIgnoredOnOpen() {
  const std::filesystem::path db_path = TestDBPath("test_db_orphan_sstable");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    FillToFlush(&db, "orphan", "v");
    assert(WaitForSSTableCount(&db, 1));
    MustOK(db.Close());
  }

  const auto files = ListSSTables(db_path);
  assert(files.size() == 1);
  std::filesystem::copy_file(files.front(), db_path / "sst_999999.data");

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    assert(db.Stats().sstable_count == 1);
    std::string value;
    MustOK(db.Get("orphan" + Key(1), &value));
    assert(value == "v1");
  }

  std::filesystem::remove_all(db_path);
}

void TestManifestChecksumCorruption() {
  const std::filesystem::path db_path = TestDBPath("test_db_manifest_crc");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Put("k", "v"));
  }
  {
    std::fstream manifest(db_path / "MANIFEST",
                          std::ios::in | std::ios::out | std::ios::binary);
    manifest.seekp(0, std::ios::beg);
    manifest.put('V');
  }
  {
    kv::DB db(db_path);
    assert(db.Open().IsCorruption());
  }

  std::filesystem::remove_all(db_path);
}

void TestWALChecksumCorruption() {
  const std::filesystem::path db_path = TestDBPath("test_db_wal_crc");
  std::filesystem::remove_all(db_path);
  std::filesystem::create_directories(db_path);
  WriteManifest(db_path, 2, 1);

  {
    kv::WALWriter wal(db_path / "wal_000001.log");
    MustOK(wal.Open());
    MustOK(wal.AppendPut("crc_key", 1, "crc_value"));
    wal.Close();
  }
  {
    std::fstream wal(db_path / "wal_000001.log",
                     std::ios::binary | std::ios::in | std::ios::out);
    wal.seekp(-1, std::ios::end);
    wal.put('X');
  }
  {
    kv::DB db(db_path);
    assert(db.Open().IsCorruption());
  }

  std::filesystem::remove_all(db_path);
}

void TestBadSSTableMagicReturnsCorruption() {
  const std::filesystem::path db_path = TestDBPath("test_db_bad_sst");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    FillToFlush(&db, "s", "v");
  }

  {
    std::fstream file(FirstSSTablePath(db_path),
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-1, std::ios::end);
    file.put('\0');
  }

  {
    kv::DB db(db_path);
    assert(db.Open().IsCorruption());
  }

  std::filesystem::remove_all(db_path);
}

void TestSSTableBlockChecksumCorruption() {
  const std::filesystem::path db_path = TestDBPath("test_db_sst_crc");
  std::filesystem::remove_all(db_path);

  {
    kv::Options options;
    options.db_path = db_path;
    options.block_size = 128;
    kv::DB db(options);
    MustOK(db.Open());
    FillToFlush(&db, "crc", "v");
  }
  {
    std::fstream file(FirstSSTablePath(db_path),
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(12, std::ios::beg);
    file.put('Z');
  }
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    assert(db.Get("crc" + Key(0), &value).IsCorruption());
  }

  std::filesystem::remove_all(db_path);
}

void TestBloomFilterAndBlockCacheStats() {
  const std::filesystem::path db_path = TestDBPath("test_db_filter_cache");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.block_size = 128;
  options.block_cache_capacity = 1;
  kv::DB db(options);
  MustOK(db.Open());
  FillToFlush(&db, "bc", "v");
  assert(WaitForSSTableCount(&db, 1));

  std::string value;
  assert(db.Get("definitely_missing", &value).IsNotFound());
  kv::DBStats stats = db.Stats();
  assert(stats.bloom_filtered > 0);

  MustOK(db.Get("bc" + Key(10), &value));
  stats = db.Stats();
  assert(stats.cache_misses > 0);
  MustOK(db.Get("bc" + Key(10), &value));
  stats = db.Stats();
  assert(stats.cache_hits > 0);
  assert(stats.block_reads > 0);

  std::filesystem::remove_all(db_path);
}

void TestBlockSSTableMultipleBlocks() {
  const std::filesystem::path db_path = TestDBPath("test_db_blocks");
  std::filesystem::remove_all(db_path);

  {
    kv::Options options;
    options.db_path = db_path;
    options.block_size = 96;
    kv::DB db(options);
    MustOK(db.Open());
    FillToFlush(&db, "blk", std::string(20, 'v'));
    assert(WaitForSSTableCount(&db, 1));

    std::string value;
    MustOK(db.Get("blk" + Key(0), &value));
    MustOK(db.Get("blk" + Key(500), &value));
    MustOK(db.Get("blk" + Key(1023), &value));
  }
  {
    kv::Options options;
    options.db_path = db_path;
    options.block_size = 96;
    kv::DB db(options);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("blk" + Key(0), &value));
    MustOK(db.Get("blk" + Key(500), &value));
    MustOK(db.Get("blk" + Key(1023), &value));
  }

  std::filesystem::remove_all(db_path);
}

void TestCompaction() {
  const std::filesystem::path db_path = TestDBPath("test_db_compact");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 100;
  kv::DB db(options);
  MustOK(db.Open());
  MustOK(db.Put("shared", "v1"));
  FillToFlush(&db, "c1", "v");
  MustOK(db.Put("shared", "v2"));
  FillToFlush(&db, "c2", "v");
  MustOK(db.Put("deleted", "value"));
  FillToFlush(&db, "c3", "v");
  MustOK(db.Delete("deleted"));
  FillToFlush(&db, "c4", "v");

  assert(WaitForSSTableCount(&db, 4));
  const auto old_sstables = ListSSTables(db_path);
  assert(old_sstables.size() >= 4);
  MustOK(db.Compact());
  assert(db.Stats().sstable_count == 1);
  assert(db.Stats().compaction_count == 1);
  for (const auto& old_sstable : old_sstables) {
    assert(!std::filesystem::exists(old_sstable));
  }

  std::string value;
  MustOK(db.Get("shared", &value));
  assert(value == "v2");
  assert(db.Get("deleted", &value).IsNotFound());

  {
    kv::DB reopened(db_path);
    MustOK(reopened.Open());
    MustOK(reopened.Get("shared", &value));
    assert(value == "v2");
    assert(reopened.Get("deleted", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestAutomaticCompaction() {
  const std::filesystem::path db_path = TestDBPath("test_db_auto_compact");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 1;
  kv::DB db(options);
  MustOK(db.Open());
  FillToFlush(&db, "a1", "v");
  FillToFlush(&db, "a2", "v");
  assert(WaitForCompactionCount(&db, 1));
  assert(db.Stats().sstable_count == 1);
  assert(db.Stats().compaction_count == 1);

  std::filesystem::remove_all(db_path);
}

void TestLeveledCompactionL0ToL1() {
  const std::filesystem::path db_path = TestDBPath("test_db_l0_l1");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 1;
  kv::DB db(options);
  MustOK(db.Open());
  FillToFlush(&db, "l0a", "v");
  FillToFlush(&db, "l0b", "v");
  assert(WaitForCompactionCount(&db, 1));

  kv::DBStats stats = db.Stats();
  assert(stats.level0_sstable_count == 0);
  assert(stats.level1_sstable_count >= 1);
  assert(stats.level2_sstable_count == 0);

  std::string value;
  MustOK(db.Get("l0a" + Key(20), &value));
  assert(value == "v20");
  MustOK(db.Get("l0b" + Key(20), &value));
  assert(value == "v20");

  std::filesystem::remove_all(db_path);
}

void TestLeveledCompactionL1ToL2AndTombstones() {
  const std::filesystem::path db_path = TestDBPath("test_db_l1_l2");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 1;
  kv::DB db(options);
  MustOK(db.Open());

  MustOK(db.Put("shared", "v1"));
  MustOK(db.Put("deleted", "live"));
  FillToFlush(&db, "round1", "v");
  MustOK(db.Put("shared", "v2"));
  MustOK(db.Delete("deleted"));
  FillToFlush(&db, "round2", "v");
  assert(WaitForCompactionCount(&db, 1));

  FillToFlush(&db, "round3", "v");
  FillToFlush(&db, "round4", "v");
  assert(WaitForCompactionCount(&db, 2));

  kv::DBStats stats = db.Stats();
  assert(stats.level2_sstable_count >= 1);

  std::string value;
  MustOK(db.Get("shared", &value));
  assert(value == "v2");
  assert(db.Get("deleted", &value).IsNotFound());

  {
    kv::DB reopened(db_path);
    MustOK(reopened.Open());
    MustOK(reopened.Get("shared", &value));
    assert(value == "v2");
    assert(reopened.Get("deleted", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestCompactionUsesStreamingIterators() {
  const std::filesystem::path db_path = TestDBPath("test_db_streaming_compaction");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 100;
  kv::DB db(options);
  MustOK(db.Open());

  MustOK(db.Put("shared", "old"));
  FillToFlush(&db, "stream_a", "v");
  MustOK(db.Put("shared", "new"));
  FillToFlush(&db, "stream_b", "v");
  assert(WaitForSSTableCount(&db, 2));

  const auto before = db.Stats().sstable_full_scans;
  MustOK(db.Compact());
  const auto after = db.Stats().sstable_full_scans;
  assert(after == before);

  std::string value;
  MustOK(db.Get("shared", &value));
  assert(value == "new");

  std::filesystem::remove_all(db_path);
}

void TestSnapshotReadStability() {
  const std::filesystem::path db_path = TestDBPath("test_db_snapshot");
  std::filesystem::remove_all(db_path);

  kv::DB db(db_path);
  MustOK(db.Open());
  MustOK(db.Put("k", "v1"));
  const kv::Snapshot* snapshot = db.GetSnapshot();
  MustOK(db.Put("k", "v2"));
  MustOK(db.Delete("k"));

  std::string value;
  kv::ReadOptions snapshot_read;
  snapshot_read.snapshot = snapshot;
  MustOK(db.Get("k", &value, snapshot_read));
  assert(value == "v1");
  assert(db.Get("k", &value).IsNotFound());
  db.ReleaseSnapshot(snapshot);

  std::filesystem::remove_all(db_path);
}

void TestSnapshotAcrossFlushAndCompaction() {
  const std::filesystem::path db_path = TestDBPath("test_db_snapshot_compact");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 100;
  kv::DB db(options);
  MustOK(db.Open());
  MustOK(db.Put("shared", "v1"));
  FillToFlush(&db, "snap_old", "v");
  assert(WaitForSSTableCount(&db, 1));
  const kv::Snapshot* snapshot = db.GetSnapshot();
  MustOK(db.Put("shared", "v2"));
  FillToFlush(&db, "snap_new", "v");
  assert(WaitForSSTableCount(&db, 2));

  MustOK(db.Compact());
  std::string value;
  kv::ReadOptions snapshot_read;
  snapshot_read.snapshot = snapshot;
  MustOK(db.Get("shared", &value, snapshot_read));
  assert(value == "v1");
  MustOK(db.Get("shared", &value));
  assert(value == "v2");

  db.ReleaseSnapshot(snapshot);
  MustOK(db.Compact());
  MustOK(db.Get("shared", &value));
  assert(value == "v2");

  std::filesystem::remove_all(db_path);
}

void TestIteratorLatestAndSnapshotViews() {
  const std::filesystem::path db_path = TestDBPath("test_db_iterator");
  std::filesystem::remove_all(db_path);

  kv::DB db(db_path);
  MustOK(db.Open());
  MustOK(db.Put("a", "old_a"));
  MustOK(db.Put("b", "old_b"));
  const kv::Snapshot* snapshot = db.GetSnapshot();
  MustOK(db.Put("a", "new_a"));
  MustOK(db.Delete("b"));
  MustOK(db.Put("c", "new_c"));

  kv::ReadOptions snapshot_read;
  snapshot_read.snapshot = snapshot;
  auto snapshot_it = db.NewIterator(snapshot_read);
  snapshot_it->SeekToFirst();
  assert(snapshot_it->Valid());
  assert(snapshot_it->key() == "a");
  assert(snapshot_it->value() == "old_a");
  snapshot_it->Next();
  assert(snapshot_it->Valid());
  assert(snapshot_it->key() == "b");
  assert(snapshot_it->value() == "old_b");
  snapshot_it->Next();
  assert(!snapshot_it->Valid());
  MustOK(snapshot_it->status());

  auto latest_it = db.NewIterator();
  latest_it->SeekToFirst();
  assert(latest_it->Valid());
  assert(latest_it->key() == "a");
  assert(latest_it->value() == "new_a");
  latest_it->Next();
  assert(latest_it->Valid());
  assert(latest_it->key() == "c");
  assert(latest_it->value() == "new_c");
  latest_it->Seek("b");
  assert(latest_it->Valid());
  assert(latest_it->key() == "c");
  latest_it->Next();
  assert(!latest_it->Valid());
  MustOK(latest_it->status());

  db.ReleaseSnapshot(snapshot);
  std::filesystem::remove_all(db_path);
}

void TestIteratorDoesNotMaterializeSSTableBlocks() {
  const std::filesystem::path db_path = TestDBPath("test_db_iterator_lazy");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.block_size = 96;
  options.level0_sstable_limit = 100;
  kv::DB db(options);
  MustOK(db.Open());
  FillToFlush(&db, "lazy", std::string(20, 'v'));
  assert(WaitForSSTableCount(&db, 1));

  const auto before = db.Stats().block_reads;
  auto iterator = db.NewIterator();
  const auto after_create = db.Stats().block_reads;
  assert(after_create == before);

  iterator->Seek("lazy" + Key(900));
  assert(iterator->Valid());
  assert(iterator->key() == "lazy" + Key(900));
  const auto after_seek = db.Stats().block_reads;
  assert(after_seek - before < 20);

  std::filesystem::remove_all(db_path);
}

void TestPrefixCompressedSSTableBlocks() {
  const std::filesystem::path db_path = TestDBPath("test_db_prefix_blocks");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.block_size = 4096;
  options.level0_sstable_limit = 100;
  {
    kv::DB db(options);
    MustOK(db.Open());
    for (size_t i = 0; i < kFlushLimit; ++i) {
      MustOK(db.Put("prefix_compressed_key_" + Key(i), "v" + std::to_string(i)));
    }
    assert(WaitForSSTableCount(&db, 1));

    const auto files = ListSSTables(db_path);
    assert(files.size() == 1);
    assert(std::filesystem::file_size(files.front()) < 33000);

    std::string value;
    const auto restart_seeks_before = db.Stats().block_restart_seeks;
    MustOK(db.Get("prefix_compressed_key_" + Key(700), &value));
    assert(value == "v700");
    assert(db.Stats().block_restart_seeks > restart_seeks_before);

    auto iterator = db.NewIterator();
    iterator->Seek("prefix_compressed_key_" + Key(900));
    assert(iterator->Valid());
    assert(iterator->key() == "prefix_compressed_key_" + Key(900));
    assert(iterator->value() == "v900");
  }

  {
    kv::DB db(options);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("prefix_compressed_key_" + Key(512), &value));
    assert(value == "v512");
  }

  std::filesystem::remove_all(db_path);
}

void TestConcurrentPutGetAndWriteBatch() {
  const std::filesystem::path db_path = TestDBPath("test_db_concurrent_write");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.memtable_entries_limit = 32;
  options.level0_sstable_limit = 100;
  kv::DB db(options);
  MustOK(db.Open());

  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&db, &failed, t] {
      for (int i = 0; i < 100; ++i) {
        kv::WriteBatch batch;
        batch.Put("thread" + std::to_string(t) + "_a_" + std::to_string(i),
                  "va" + std::to_string(i));
        batch.Put("thread" + std::to_string(t) + "_b_" + std::to_string(i),
                  "vb" + std::to_string(i));
        if (!db.Write(batch).ok()) {
          failed = true;
          return;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  assert(!failed);
  MustOK(db.Close());

  kv::DB reopened(options);
  MustOK(reopened.Open());
  for (int t = 0; t < 4; ++t) {
    for (int i = 0; i < 100; ++i) {
      std::string value;
      MustOK(reopened.Get("thread" + std::to_string(t) + "_a_" + std::to_string(i), &value));
      assert(value == "va" + std::to_string(i));
      MustOK(reopened.Get("thread" + std::to_string(t) + "_b_" + std::to_string(i), &value));
      assert(value == "vb" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(db_path);
}

void TestConcurrentGetDuringFlushAndCompaction() {
  const std::filesystem::path db_path = TestDBPath("test_db_concurrent_get");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.memtable_entries_limit = 16;
  options.level0_sstable_limit = 1;
  kv::DB db(options);
  MustOK(db.Open());
  MustOK(db.Put("stable", "v0"));

  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::thread reader([&] {
    while (!stop.load()) {
      std::string value;
      kv::Status status = db.Get("stable", &value);
      if (!status.ok() || value != "v0") {
        failed = true;
        return;
      }
    }
  });

  for (int round = 0; round < 8; ++round) {
    for (int i = 0; i < 64; ++i) {
      MustOK(db.Put("cg_" + std::to_string(round) + "_" + std::to_string(i),
                    "v" + std::to_string(i)));
    }
  }
  MustOK(db.Compact());
  stop = true;
  reader.join();
  assert(!failed);

  std::string value;
  MustOK(db.Get("stable", &value));
  assert(value == "v0");
  MustOK(db.Close());

  std::filesystem::remove_all(db_path);
}

void TestCloseWhileWriterIsActive() {
  const std::filesystem::path db_path = TestDBPath("test_db_concurrent_close");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.memtable_entries_limit = 16;
  options.level0_sstable_limit = 100;
  auto db = std::make_unique<kv::DB>(options);
  MustOK(db->Open());

  std::atomic<int> successful_writes{0};
  std::thread writer([&] {
    for (int i = 0; i < 500; ++i) {
      kv::Status status = db->Put("close_key_" + std::to_string(i),
                                  "v" + std::to_string(i));
      if (!status.ok()) {
        return;
      }
      successful_writes = i + 1;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  MustOK(db->Close());
  writer.join();
  const int written = successful_writes.load();
  db.reset();

  kv::DB reopened(options);
  MustOK(reopened.Open());
  for (int i = 0; i < written; ++i) {
    std::string value;
    MustOK(reopened.Get("close_key_" + std::to_string(i), &value));
    assert(value == "v" + std::to_string(i));
  }

  std::filesystem::remove_all(db_path);
}

void TestSequenceRecoveryAfterReopen() {
  const std::filesystem::path db_path = TestDBPath("test_db_sequence_recovery");
  std::filesystem::remove_all(db_path);

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Put("k", "v1"));
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    const kv::Snapshot* snapshot = db.GetSnapshot();
    MustOK(db.Put("k", "v2"));

    std::string value;
    kv::ReadOptions snapshot_read;
    snapshot_read.snapshot = snapshot;
    MustOK(db.Get("k", &value, snapshot_read));
    assert(value == "v1");
    MustOK(db.Get("k", &value));
    assert(value == "v2");
    db.ReleaseSnapshot(snapshot);
  }

  std::filesystem::remove_all(db_path);
}

}  // namespace

int main() {
  TestBasicWALRecovery();
  TestFlushAndSSTableRecovery();
  TestNewestSSTableWins();
  TestDeleteFlushRecovery();
  TestInvalidInput();
  TestUpdateDeleteAndValues();
  TestWriteBatchOperationsAndRecovery();
  TestTruncatedLastWALBatchIsIgnored();
  TestImmutableMemTableWriteAndReadOrdering();
  TestCloseFlushesActiveAndImmutableMemTables();
  TestFlushFailureKeepsWALForRecovery();
  TestManifestRecoveryAndFallback();
  TestManifestEditLogReplayAndTruncation();
  TestOrphanSSTableIgnoredOnOpen();
  TestManifestChecksumCorruption();
  TestBadWALReturnsCorruption();
  TestWALChecksumCorruption();
  TestBadSSTableMagicReturnsCorruption();
  TestSSTableBlockChecksumCorruption();
  TestBloomFilterAndBlockCacheStats();
  TestBlockSSTableMultipleBlocks();
  TestCompaction();
  TestAutomaticCompaction();
  TestLeveledCompactionL0ToL1();
  TestLeveledCompactionL1ToL2AndTombstones();
  TestCompactionUsesStreamingIterators();
  TestSnapshotReadStability();
  TestSnapshotAcrossFlushAndCompaction();
  TestIteratorLatestAndSnapshotViews();
  TestIteratorDoesNotMaterializeSSTableBlocks();
  TestPrefixCompressedSSTableBlocks();
  TestConcurrentPutGetAndWriteBatch();
  TestConcurrentGetDuringFlushAndCompaction();
  TestCloseWhileWriterIsActive();
  TestSequenceRecoveryAfterReopen();

  std::cout << "kv_test passed\n";
  return 0;
}
