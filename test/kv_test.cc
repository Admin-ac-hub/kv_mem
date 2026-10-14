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

#include "db.h"
#include "format.h"
#include "test_support.h"

namespace {

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

bool WaitForWALCount(const std::filesystem::path& path, size_t expected) {
  for (int i = 0; i < 200; ++i) {
    if (CountWALFiles(path) == expected) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::filesystem::path FirstSSTablePath(const std::filesystem::path& db_path) {
  auto files = ListSSTables(db_path);
  CHECK(!files.empty());
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
  body << "version_edit 1\n";
  body << "next_file_number " << next_file_number << '\n';
  body << "wal_file_number " << wal_file_number << '\n';
  body << "last_sequence 0\n";
  const std::string payload = body.str();
  std::ofstream manifest(db_path / "MANIFEST", std::ios::binary | std::ios::trunc);
  manifest << "record " << payload.size() << ' ' << kv::CRC32(payload) << '\n' << payload;
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
    CHECK(value == "v1");
    CHECK(db.Get("k2", &value).IsNotFound());
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("k1", &value));
    CHECK(value == "v1");
    CHECK(db.Get("k2", &value).IsNotFound());
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

    CHECK(WaitForSSTableCount(&db, 1));
    CHECK(ListSSTables(db_path).size() == 1);
    // Publication precedes asynchronous WAL cleanup; observe both separately.
    CHECK(WaitForWALCount(db_path, 1));

    std::string value;
    MustOK(db.Get("a" + Key(0), &value));
    CHECK(value == "v0");
    MustOK(db.Get("a" + Key(1023), &value));
    CHECK(value == "v1023");

    MustOK(db.Put("wal_only", "still_in_wal"));
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("a" + Key(100), &value));
    CHECK(value == "v100");
    MustOK(db.Get("wal_only", &value));
    CHECK(value == "still_in_wal");
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
    CHECK(WaitForSSTableCount(&db, 1));
    MustOK(db.Put("shared", "new"));
    FillToFlush(&db, "new", "v");
    CHECK(WaitForSSTableCount(&db, 2));

    CHECK(ListSSTables(db_path).size() == 2);

    std::string value;
    MustOK(db.Get("shared", &value));
    CHECK(value == "new");
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("shared", &value));
    CHECK(value == "new");
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
    CHECK(db.Get("gone", &value).IsNotFound());
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    CHECK(db.Get("gone", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestInputValidationAndBinaryValues() {
  const std::filesystem::path db_path = TestDBPath("test_db_invalid");
  std::filesystem::remove_all(db_path);

  kv::DB db(db_path);
  MustOK(db.Open());

  CHECK(!db.Put("bad\tkey", "value").ok());
  CHECK(!db.Put("bad\nkey", "value").ok());
  CHECK(!db.Delete("bad\tkey").ok());
  std::string binary_value = "tab\tnewline\nnul";
  binary_value.push_back('\0');
  binary_value += "__DELETE__";
  MustOK(db.Put("key", binary_value));
  std::string value;
  MustOK(db.Get("key", &value));
  CHECK(value == binary_value);
  MustOK(db.Close());

  {
    kv::DB reopened(db_path);
    MustOK(reopened.Open());
    MustOK(reopened.Get("key", &value));
    CHECK(value == binary_value);
  }

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
  CHECK(value == "v2");
  CHECK(db.Get("missing", &value).IsNotFound());
  MustOK(db.Get("empty", &value));
  CHECK(value.empty());
  MustOK(db.Get("large", &value));
  CHECK(value.size() == 4096);

  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestWriteBatchOperationsAndRecovery() {
  const std::filesystem::path db_path = TestDBPath("test_db_write_batch");
  std::filesystem::remove_all(db_path);

  {
    kv::WriteBatch batch;
    CHECK(batch.Count() == 0);
    batch.Put("a", "v1");
    batch.Put("b", "v2");
    batch.Delete("missing");
    CHECK(batch.Count() == 3);

    kv::WriteBatch decoded;
    MustOK(decoded.Decode(batch.Encode()));
    CHECK(decoded.Count() == 3);

    kv::DB db(db_path);
    MustOK(db.Open());
    MustOK(db.Write(decoded));

    kv::WriteBatch overwrite;
    overwrite.Put("a", "v3");
    overwrite.Delete("b");
    MustOK(db.Write(overwrite));

    std::string value;
    MustOK(db.Get("a", &value));
    CHECK(value == "v3");
    CHECK(db.Get("b", &value).IsNotFound());
    CHECK(db.Get("missing", &value).IsNotFound());

    overwrite.Clear();
    CHECK(overwrite.Count() == 0);
  }

  {
    kv::DB db(db_path);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("a", &value));
    CHECK(value == "v3");
    CHECK(db.Get("b", &value).IsNotFound());
    CHECK(db.Get("missing", &value).IsNotFound());
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
    CHECK(value == "yes");
    CHECK(db.Get("partial_a", &value).IsNotFound());
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
    CHECK(value == "new");
    MustOK(db.Get("filler1", &value));
    CHECK(value == "v1");
    MustOK(db.Get("active", &value));
    CHECK(value == "v3");

    CHECK(WaitForSSTableCount(&db, 2));
  }

  {
    kv::DB db(options);
    MustOK(db.Open());

    std::string value;
    MustOK(db.Get("shared", &value));
    CHECK(value == "new");
    MustOK(db.Get("filler1", &value));
    CHECK(value == "v1");
    MustOK(db.Get("active", &value));
    CHECK(value == "v3");
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
    CHECK(db.Stats().sstable_count >= 3);
  }

  {
    kv::DB db(options);
    MustOK(db.Open());
    for (int i = 0; i < 8; ++i) {
      std::string value;
      MustOK(db.Get("k" + std::to_string(i), &value));
      CHECK(value == "v" + std::to_string(i));
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
    CHECK(CountWALFiles(db_path) >= 1);
    CHECK(!db.Close().ok());
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
    CHECK(value == "va");
    MustOK(db.Get("survives_b", &value));
    CHECK(value == "vb");
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
  CHECK(std::filesystem::exists(db_path / "MANIFEST"));

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("m" + Key(1), &value));
    CHECK(value == "v1");
  }

  std::filesystem::remove(db_path / "MANIFEST");
  {
    kv::DB db(db_path);
    CHECK(db.Open().IsIOError());
  }

  std::filesystem::remove(db_path / "CURRENT");
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    CHECK(std::filesystem::exists(db_path / "MANIFEST"));
    std::string value;
    MustOK(db.Get("m" + Key(2), &value));
    CHECK(value == "v2");
  }

  {
    std::ofstream out(db_path / "MANIFEST", std::ios::trunc);
    out << "broken manifest\n";
  }
  {
    kv::DB db(db_path);
    CHECK(db.Open().IsCorruption());
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

  {
    kv::DB db(db_path);
    CHECK(db.Open().IsCorruption());
  }
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
    CHECK(WaitForSSTableCount(&db, 1));
    FillToFlush(&db, "e2", "v");
    CHECK(WaitForSSTableCount(&db, 2));
    MustOK(db.Close());
  }

  CHECK(std::filesystem::exists(db_path / "CURRENT"));
  CHECK(CountManifestRecords(db_path) >= 3);
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("e1" + Key(10), &value));
    CHECK(value == "v10");
    MustOK(db.Get("e2" + Key(10), &value));
    CHECK(value == "v10");
  }

  const std::filesystem::path manifest_path = db_path / "MANIFEST";
  const auto original_size = std::filesystem::file_size(manifest_path);
  std::filesystem::resize_file(manifest_path, original_size - 5);
  {
    kv::DB db(db_path);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("e1" + Key(20), &value));
    CHECK(value == "v20");
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
    CHECK(WaitForSSTableCount(&db, 1));
    MustOK(db.Close());
  }

  const auto files = ListSSTables(db_path);
  CHECK(files.size() == 1);
  std::filesystem::copy_file(files.front(), db_path / "sst_999999.data");

  {
    kv::DB db(db_path);
    MustOK(db.Open());
    CHECK(db.Stats().sstable_count == 1);
    std::string value;
    MustOK(db.Get("orphan" + Key(1), &value));
    CHECK(value == "v1");
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
    CHECK(db.Open().IsCorruption());
  }

  std::filesystem::remove_all(db_path);
}

void TestWALRequiresBatchEnvelope() {
  const auto db_path = TestDBPath("test_db_wal_batch_envelope");
  for (int variant = 0; variant < 3; ++variant) {
    std::filesystem::remove_all(db_path);
    std::filesystem::create_directories(db_path);
    WriteManifest(db_path, 2, 1);
    const auto wal_path = db_path / "wal_000001.log";

    kv::WALWriter writer(wal_path);
    MustOK(writer.Open());
    kv::WriteBatch batch;
    batch.Put("durable", "yes");
    MustOK(writer.AppendBatch(batch, 1));
    const auto second_record_offset = std::filesystem::file_size(wal_path);
    if (variant == 2) MustOK(writer.AppendBatch(batch, 2));
    writer.Close();

    if (variant < 2) {
      // A checksum-valid single-operation record must no longer be accepted.
      const std::string value = variant == 0 ? "v" : "";
      std::string body(17, '\0');
      body[0] = static_cast<char>(variant == 0 ? kv::WriteBatchOpType::kPut
                                               : kv::WriteBatchOpType::kDelete);
      kv::EncodeFixed64(2, body.data() + 1);
      kv::EncodeFixed32(1, body.data() + 9);
      kv::EncodeFixed32(static_cast<std::uint32_t>(value.size()), body.data() + 13);
      body.append("k");
      body.append(value);
      char checksum[4];
      kv::EncodeFixed32(kv::CRC32(body), checksum);
      std::ofstream output(wal_path, std::ios::binary | std::ios::app);
      output.write(checksum, sizeof(checksum));
      output.write(body.data(), static_cast<std::streamsize>(body.size()));
      CHECK(output.good());
    } else {
      // A complete batch with a damaged magic must not be treated as a torn tail.
      std::fstream output(wal_path, std::ios::binary | std::ios::in | std::ios::out);
      output.seekp(static_cast<std::streamoff>(second_record_offset));
      output.put('X');
      CHECK(output.good());
    }

    const auto original_size = std::filesystem::file_size(wal_path);
    {
      kv::WALReader reader(wal_path);
      kv::WALBatchRecord record;
      bool eof = false;
      MustOK(reader.ReadNext(&record, &eof));
      CHECK(!eof && record.sequence == 1 && record.batch.Count() == 1);
      CHECK(reader.ReadNext(&record, &eof).IsCorruption());
      CHECK(!eof && record.batch.Count() == 0);
    }
    CHECK(std::filesystem::file_size(wal_path) == original_size);
    {
      kv::DB db(db_path);
      CHECK(db.Open().IsCorruption());
    }
    CHECK(std::filesystem::file_size(wal_path) == original_size);
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
    kv::WriteBatch batch;
    batch.Put("crc_key", "crc_value");
    MustOK(wal.AppendBatch(batch, 1));
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
    CHECK(db.Open().IsCorruption());
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
    CHECK(db.Open().IsCorruption());
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
    CHECK(db.Get("crc" + Key(0), &value).IsCorruption());
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
  CHECK(WaitForSSTableCount(&db, 1));

  std::string value;
  CHECK(db.Get("definitely_missing", &value).IsNotFound());
  kv::DBStats stats = db.Stats();
  CHECK(stats.bloom_filtered > 0);

  MustOK(db.Get("bc" + Key(10), &value));
  stats = db.Stats();
  CHECK(stats.cache_misses > 0);
  MustOK(db.Get("bc" + Key(10), &value));
  stats = db.Stats();
  CHECK(stats.cache_hits > 0);
  CHECK(stats.block_reads > 0);

  MustOK(db.Close());
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
    CHECK(WaitForSSTableCount(&db, 1));

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

  CHECK(WaitForSSTableCount(&db, 4));
  const auto old_sstables = ListSSTables(db_path);
  CHECK(old_sstables.size() >= 4);
  MustOK(db.Compact());
  CHECK(db.Stats().sstable_count == 1);
  CHECK(db.Stats().compaction_count == 1);
  for (const auto& old_sstable : old_sstables) {
    CHECK(!std::filesystem::exists(old_sstable));
  }

  std::string value;
  MustOK(db.Get("shared", &value));
  CHECK(value == "v2");
  CHECK(db.Get("deleted", &value).IsNotFound());
  MustOK(db.Close());

  {
    kv::DB reopened(db_path);
    MustOK(reopened.Open());
    MustOK(reopened.Get("shared", &value));
    CHECK(value == "v2");
    CHECK(reopened.Get("deleted", &value).IsNotFound());
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
  CHECK(WaitForCompactionCount(&db, 1));
  CHECK(db.Stats().sstable_count == 1);
  CHECK(db.Stats().compaction_count == 1);

  MustOK(db.Close());
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
  CHECK(WaitForCompactionCount(&db, 1));

  kv::DBStats stats = db.Stats();
  CHECK(stats.level0_sstable_count == 0);
  CHECK(stats.level1_sstable_count >= 1);
  CHECK(stats.level2_sstable_count == 0);

  std::string value;
  MustOK(db.Get("l0a" + Key(20), &value));
  CHECK(value == "v20");
  MustOK(db.Get("l0b" + Key(20), &value));
  CHECK(value == "v20");

  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestLeveledCompactionL1ToL2AndTombstones() {
  const std::filesystem::path db_path = TestDBPath("test_db_l1_l2");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 1;
  options.level1_size_limit_bytes = 64 * 1024;
  kv::DB db(options);
  MustOK(db.Open());

  MustOK(db.Put("shared", "v1"));
  MustOK(db.Put("deleted", "live"));
  FillToFlush(&db, "round1", "v");
  MustOK(db.Put("shared", "v2"));
  MustOK(db.Delete("deleted"));
  FillToFlush(&db, "round2", "v");
  CHECK(WaitForCompactionCount(&db, 1));

  FillToFlush(&db, "round3", "v");
  FillToFlush(&db, "round4", "v");
  CHECK(WaitForLevel2SSTable(&db));

  kv::DBStats stats = db.Stats();
  CHECK(stats.level2_sstable_count >= 1);

  std::string value;
  MustOK(db.Get("shared", &value));
  CHECK(value == "v2");
  CHECK(db.Get("deleted", &value).IsNotFound());
  MustOK(db.Close());

  {
    kv::DB reopened(db_path);
    MustOK(reopened.Open());
    MustOK(reopened.Get("shared", &value));
    CHECK(value == "v2");
    CHECK(reopened.Get("deleted", &value).IsNotFound());
  }

  std::filesystem::remove_all(db_path);
}

void TestGetUsesSequenceAcrossConcurrentCompactionAndFlush() {
  const auto path = TestDBPath("compaction_flush_sequence_order");
  std::filesystem::remove_all(path);
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool publish = false;
  kv::Options options;
  options.db_path = path;
  options.memtable_entries_limit = 3;
  options.level0_sstable_limit = 100;
  options.testing_before_compaction_publish = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    ready = true;
    cv.notify_all();
    cv.wait(lock, [&] { return publish; });
  };
  kv::DB db(options);
  MustOK(db.Open());
  kv::WriteBatch old;
  old.Delete("resurrected");
  old.Put("updated", "old");
  old.Put("removed", "old");
  MustOK(db.Write(old));
  CHECK(WaitForSSTableCount(&db, 1));
  const auto* snapshot = db.GetSnapshot();
  kv::Status compact_status;
  std::thread compactor([&] { compact_status = db.Compact(); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(2), [&] { return ready; }));
  }
  kv::WriteBatch newer;
  newer.Put("resurrected", "new");
  newer.Put("updated", "new");
  newer.Delete("removed");
  MustOK(db.Write(newer));
  CHECK(WaitForSSTableCount(&db, 2));
  {
    std::lock_guard<std::mutex> lock(mutex);
    publish = true;
  }
  cv.notify_all();
  compactor.join();
  MustOK(compact_status);
  const auto verify_latest = [&] {
    std::string value;
    MustOK(db.Get("resurrected", &value));
    CHECK(value == "new");
    MustOK(db.Get("updated", &value));
    CHECK(value == "new");
    CHECK(db.Get("removed", &value).IsNotFound());
  };
  verify_latest();
  std::string value;
  CHECK(db.Get("resurrected", &value, {snapshot}).IsNotFound());
  MustOK(db.Get("updated", &value, {snapshot}));
  CHECK(value == "old");
  MustOK(db.Get("removed", &value, {snapshot}));
  CHECK(value == "old");
  db.ReleaseSnapshot(snapshot);
  MustOK(db.Close());
  MustOK(db.Open());
  verify_latest();
  MustOK(db.Close());
  std::filesystem::remove_all(path);
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
  CHECK(WaitForSSTableCount(&db, 2));

  const auto before = db.Stats().sstable_full_scans;
  MustOK(db.Compact());
  const auto after = db.Stats().sstable_full_scans;
  CHECK(after == before);

  std::string value;
  MustOK(db.Get("shared", &value));
  CHECK(value == "new");

  MustOK(db.Close());
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
  CHECK(value == "v1");
  CHECK(db.Get("k", &value).IsNotFound());
  db.ReleaseSnapshot(snapshot);

  MustOK(db.Close());
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
  CHECK(WaitForSSTableCount(&db, 1));
  const kv::Snapshot* snapshot = db.GetSnapshot();
  MustOK(db.Put("shared", "v2"));
  FillToFlush(&db, "snap_new", "v");
  CHECK(WaitForSSTableCount(&db, 2));

  MustOK(db.Compact());
  std::string value;
  kv::ReadOptions snapshot_read;
  snapshot_read.snapshot = snapshot;
  MustOK(db.Get("shared", &value, snapshot_read));
  CHECK(value == "v1");
  MustOK(db.Get("shared", &value));
  CHECK(value == "v2");

  db.ReleaseSnapshot(snapshot);
  MustOK(db.Compact());
  MustOK(db.Get("shared", &value));
  CHECK(value == "v2");

  MustOK(db.Close());
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
  CHECK(snapshot_it->Valid());
  CHECK(snapshot_it->key() == "a");
  CHECK(snapshot_it->value() == "old_a");
  snapshot_it->Next();
  CHECK(snapshot_it->Valid());
  CHECK(snapshot_it->key() == "b");
  CHECK(snapshot_it->value() == "old_b");
  snapshot_it->Next();
  CHECK(!snapshot_it->Valid());
  MustOK(snapshot_it->status());

  auto latest_it = db.NewIterator();
  latest_it->SeekToFirst();
  CHECK(latest_it->Valid());
  CHECK(latest_it->key() == "a");
  CHECK(latest_it->value() == "new_a");
  latest_it->Next();
  CHECK(latest_it->Valid());
  CHECK(latest_it->key() == "c");
  CHECK(latest_it->value() == "new_c");
  latest_it->Seek("b");
  CHECK(latest_it->Valid());
  CHECK(latest_it->key() == "c");
  latest_it->Next();
  CHECK(!latest_it->Valid());
  MustOK(latest_it->status());

  db.ReleaseSnapshot(snapshot);
  snapshot_it.reset();
  latest_it.reset();
  MustOK(db.Close());
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
  CHECK(WaitForSSTableCount(&db, 1));

  const auto before = db.Stats().block_reads;
  auto iterator = db.NewIterator();
  const auto after_create = db.Stats().block_reads;
  CHECK(after_create == before);

  iterator->Seek("lazy" + Key(900));
  CHECK(iterator->Valid());
  CHECK(iterator->key() == "lazy" + Key(900));
  const auto after_seek = db.Stats().block_reads;
  CHECK(after_seek - before < 20);

  iterator.reset();
  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestIteratorPinsCompactedSSTables() {
  const std::filesystem::path db_path = TestDBPath("test_db_iterator_compaction_pin");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 100;
  options.block_cache_capacity = 0;
  kv::DB db(options);
  MustOK(db.Open());

  MustOK(db.Put("lifetime_target", "old_value"));
  for (size_t i = 0; i + 1 < kFlushLimit; ++i) {
    MustOK(db.Put("iterator_pin_" + Key(i), "v" + std::to_string(i)));
  }
  CHECK(WaitForSSTableCount(&db, 1));

  const auto old_sstables = ListSSTables(db_path);
  CHECK(old_sstables.size() == 1);
  auto iterator = db.NewIterator();
  MustOK(db.Compact());
  for (const auto& old_sstable : old_sstables) {
    CHECK(!std::filesystem::exists(old_sstable));
  }

  iterator->Seek("lifetime_target");
  CHECK(iterator->Valid());
  CHECK(iterator->key() == "lifetime_target");
  CHECK(iterator->value() == "old_value");
  MustOK(iterator->status());

  iterator.reset();
  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestGetPinsCompactedSSTableFile() {
  const std::filesystem::path db_path = TestDBPath("test_db_get_compaction_pin");
  std::filesystem::remove_all(db_path);

  std::mutex barrier_mutex;
  std::condition_variable barrier_cv;
  bool read_version_pinned = false;
  bool resume_read = false;

  kv::Options options;
  options.db_path = db_path;
  options.level0_sstable_limit = 100;
  options.block_cache_capacity = 0;
  options.testing_after_read_version_pin = [&] {
    std::unique_lock<std::mutex> lock(barrier_mutex);
    read_version_pinned = true;
    barrier_cv.notify_one();
    barrier_cv.wait(lock, [&] { return resume_read; });
  };

  kv::DB db(options);
  MustOK(db.Open());
  MustOK(db.Put("pinned_get_target", "pinned_value"));
  for (size_t i = 0; i + 1 < kFlushLimit; ++i) {
    MustOK(db.Put("get_pin_" + Key(i), "v" + std::to_string(i)));
  }
  CHECK(WaitForSSTableCount(&db, 1));
  const auto old_sstables = ListSSTables(db_path);
  CHECK(old_sstables.size() == 1);

  kv::Status read_status;
  std::string read_value;
  std::thread reader([&] { read_status = db.Get("pinned_get_target", &read_value); });
  {
    std::unique_lock<std::mutex> lock(barrier_mutex);
    CHECK(barrier_cv.wait_for(lock, std::chrono::seconds(2),
                              [&] { return read_version_pinned; }));
  }

  MustOK(db.Compact());
  for (const auto& old_sstable : old_sstables) {
    CHECK(!std::filesystem::exists(old_sstable));
  }
  {
    std::lock_guard<std::mutex> lock(barrier_mutex);
    resume_read = true;
  }
  barrier_cv.notify_one();
  reader.join();

  MustOK(read_status);
  CHECK(read_value == "pinned_value");
  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestConcurrentSSTableReadsAndStats() {
  const std::filesystem::path db_path = TestDBPath("test_db_concurrent_sstable_stats");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.block_size = 128;
  options.block_cache_capacity = 8;
  options.level0_sstable_limit = 100;
  kv::DB db(options);
  MustOK(db.Open());
  FillToFlush(&db, "stats", "v");
  CHECK(WaitForSSTableCount(&db, 1));

  constexpr int kReaderCount = 4;
  constexpr int kReadsPerThread = 500;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  for (int thread_index = 0; thread_index < kReaderCount; ++thread_index) {
    readers.emplace_back([&, thread_index] {
      ready.fetch_add(1);
      while (!start.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kReadsPerThread; ++i) {
        const size_t key_index =
            static_cast<size_t>((thread_index * 131 + i * 17) % kFlushLimit);
        std::string value;
        const kv::Status status = db.Get("stats" + Key(key_index), &value);
        if (!status.ok() || value != "v" + std::to_string(key_index) ||
            db.Stats().sstable_count != 1) {
          failed = true;
          return;
        }
      }
    });
  }
  while (ready.load() != kReaderCount) {
    std::this_thread::yield();
  }
  start = true;
  for (auto& reader : readers) {
    reader.join();
  }

  CHECK(!failed.load());
  const kv::DBStats stats = db.Stats();
  CHECK(stats.cache_hits > 0);
  CHECK(stats.cache_misses > 0);
  CHECK(stats.block_reads > 0);
  MustOK(db.Close());
  std::filesystem::remove_all(db_path);
}

void TestRestartSeekAcrossKeyVersions() {
  const std::filesystem::path db_path = TestDBPath("test_db_restart_versions");
  std::filesystem::remove_all(db_path);

  kv::Options options;
  options.db_path = db_path;
  options.memtable_entries_limit = 48;
  options.block_size = 128;
  options.block_cache_capacity = 0;
  options.level0_sstable_limit = 100;

  kv::DB db(options);
  MustOK(db.Open());
  const kv::Snapshot* version_16 = nullptr;
  const kv::Snapshot* version_32 = nullptr;
  for (int version = 1; version <= 48; ++version) {
    MustOK(db.Put("versioned", "v" + std::to_string(version)));
    if (version == 16) {
      version_16 = db.GetSnapshot();
    } else if (version == 32) {
      version_32 = db.GetSnapshot();
    }
  }
  CHECK(WaitForSSTableCount(&db, 1));

  std::string value;
  kv::ReadOptions read_options;
  read_options.snapshot = version_16;
  MustOK(db.Get("versioned", &value, read_options));
  CHECK(value == "v16");
  read_options.snapshot = version_32;
  MustOK(db.Get("versioned", &value, read_options));
  CHECK(value == "v32");
  MustOK(db.Get("versioned", &value));
  CHECK(value == "v48");
  CHECK(db.Stats().block_restart_seeks >= 3);

  db.ReleaseSnapshot(version_16);
  db.ReleaseSnapshot(version_32);
  MustOK(db.Close());
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
    CHECK(WaitForSSTableCount(&db, 1));

    const auto files = ListSSTables(db_path);
    CHECK(files.size() == 1);
    CHECK(std::filesystem::file_size(files.front()) < 33000);

    std::string value;
    const auto restart_seeks_before = db.Stats().block_restart_seeks;
    MustOK(db.Get("prefix_compressed_key_" + Key(700), &value));
    CHECK(value == "v700");
    CHECK(db.Stats().block_restart_seeks > restart_seeks_before);

    auto iterator = db.NewIterator();
    iterator->Seek("prefix_compressed_key_" + Key(900));
    CHECK(iterator->Valid());
    CHECK(iterator->key() == "prefix_compressed_key_" + Key(900));
    CHECK(iterator->value() == "v900");
  }

  {
    kv::DB db(options);
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("prefix_compressed_key_" + Key(512), &value));
    CHECK(value == "v512");
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
  CHECK(!failed);
  MustOK(db.Close());

  kv::DB reopened(options);
  MustOK(reopened.Open());
  for (int t = 0; t < 4; ++t) {
    for (int i = 0; i < 100; ++i) {
      std::string value;
      MustOK(reopened.Get("thread" + std::to_string(t) + "_a_" + std::to_string(i), &value));
      CHECK(value == "va" + std::to_string(i));
      MustOK(reopened.Get("thread" + std::to_string(t) + "_b_" + std::to_string(i), &value));
      CHECK(value == "vb" + std::to_string(i));
    }
  }

  MustOK(reopened.Close());
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
  CHECK(!failed);

  std::string value;
  MustOK(db.Get("stable", &value));
  CHECK(value == "v0");
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
    CHECK(value == "v" + std::to_string(i));
  }

  MustOK(reopened.Close());
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
    CHECK(value == "v1");
    MustOK(db.Get("k", &value));
    CHECK(value == "v2");
    db.ReleaseSnapshot(snapshot);
  }

  std::filesystem::remove_all(db_path);
}

void TestLargeValueCompactionByteBudget() {
  for (const bool small_budget : {false, true}) {
    const auto path = TestDBPath(small_budget ? "vector_small_l1" : "vector_default_l1");
    std::filesystem::remove_all(path);
    kv::Options options;
    options.db_path = path;
    options.memtable_entries_limit = 2;
    options.level0_sstable_limit = 1;
    if (small_budget) {
      options.level1_size_limit_bytes = 1024;
    }
    kv::DB db(options);
    MustOK(db.Open());
    for (size_t i = 0; i < 4; ++i) {
      MustOK(db.Put(Key(i), std::string(3093, static_cast<char>(i))));
    }
    // Close joins the worker after it drains flushes and automatic compactions.
    MustOK(db.Close());
    const auto stats = db.Stats();
    CHECK(stats.flush_count == 2);
    CHECK(stats.level0_sstable_count == 0);
    CHECK(stats.level1_sstable_count == (small_budget ? 0 : 1));
    CHECK(stats.level2_sstable_count == (small_budget ? 1 : 0));
    CHECK(stats.compaction_count == (small_budget ? 2 : 1));
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get(Key(3), &value));
    CHECK(value == std::string(3093, 3));
    MustOK(db.Close());
    std::filesystem::remove_all(path);
  }
}

void TestCoreReadLifecycleAndSnapshotOwnership() {
  const auto path = TestDBPath("core_read_lifecycle");
  const auto other_path = TestDBPath("core_read_lifecycle_other");
  std::filesystem::remove_all(path);
  std::filesystem::remove_all(other_path);
  kv::DB db(path), other(other_path);
  std::string value;
  CHECK(db.GetSnapshot() == nullptr);
  CHECK(db.Get("key", &value).IsIOError());
  CHECK(db.NewIterator()->status().IsIOError());
  MustOK(db.Open()); MustOK(other.Open());
  MustOK(db.Put("key", "old"));
  const auto* snapshot = db.GetSnapshot();
  const auto* foreign = other.GetSnapshot();
  CHECK(!db.Get("key", &value, kv::ReadOptions{foreign}).ok());
  CHECK(!db.NewIterator(kv::ReadOptions{foreign})->status().ok());
  db.ReleaseSnapshot(snapshot);
  const auto* fresh = db.GetSnapshot(); CHECK(fresh != snapshot);
  CHECK(!db.Get("key", &value, kv::ReadOptions{snapshot}).ok());
  MustOK(db.Close());
  CHECK(db.GetSnapshot() == nullptr);
  CHECK(db.Get("key", &value).IsIOError());
  MustOK(db.Open());
  CHECK(!db.Get("key", &value, kv::ReadOptions{fresh}).ok());
  MustOK(db.Get("key", &value)); CHECK(value == "old");
  db.ReleaseSnapshot(fresh); other.ReleaseSnapshot(foreign);
  MustOK(db.Close()); MustOK(other.Close());
  std::filesystem::remove_all(path);
  std::filesystem::remove_all(other_path);
}

}  // namespace

int main() {
  TestBasicWALRecovery();
  TestFlushAndSSTableRecovery();
  TestNewestSSTableWins();
  TestDeleteFlushRecovery();
  TestInputValidationAndBinaryValues();
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
  TestWALRequiresBatchEnvelope();
  TestWALChecksumCorruption();
  TestBadSSTableMagicReturnsCorruption();
  TestSSTableBlockChecksumCorruption();
  TestBloomFilterAndBlockCacheStats();
  TestBlockSSTableMultipleBlocks();
  TestCompaction();
  TestAutomaticCompaction();
  TestLeveledCompactionL0ToL1();
  TestLeveledCompactionL1ToL2AndTombstones();
  TestGetUsesSequenceAcrossConcurrentCompactionAndFlush();
  TestLargeValueCompactionByteBudget();
  TestCompactionUsesStreamingIterators();
  TestSnapshotReadStability();
  TestSnapshotAcrossFlushAndCompaction();
  TestIteratorLatestAndSnapshotViews();
  TestIteratorDoesNotMaterializeSSTableBlocks();
  TestIteratorPinsCompactedSSTables();
  TestGetPinsCompactedSSTableFile();
  TestConcurrentSSTableReadsAndStats();
  TestRestartSeekAcrossKeyVersions();
  TestPrefixCompressedSSTableBlocks();
  TestConcurrentPutGetAndWriteBatch();
  TestConcurrentGetDuringFlushAndCompaction();
  TestCloseWhileWriterIsActive();
  TestSequenceRecoveryAfterReopen();
  TestCoreReadLifecycleAndSnapshotOwnership();

  std::cout << "kv_test passed\n";
  return 0;
}
