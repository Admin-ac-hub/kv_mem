#include "db.h"
#include "format.h"
#include "test_support.h"

#include <atomic>
#include <fstream>
#include <limits>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;

namespace {

void TestOptionsAndPaths() {
  const auto path = TestDBPath("boundary_options");
  for (int field = 0; field < 7; ++field) {
    std::filesystem::remove_all(path);
    kv::Options options; options.db_path = path;
    switch (field) {
      case 0: options.memtable_entries_limit = 0; break;
      case 1: options.level0_sstable_limit = 0; break;
      case 2: options.level1_size_limit_bytes = 0; break;
      case 3: options.block_size = 0; break;
      case 4: options.bloom_bits_per_key = 0; break;
      case 5: options.bloom_bits_per_key = 65; break;
      case 6: options.block_size = std::numeric_limits<size_t>::max(); break;
    }
    kv::DB db(options);
    CHECK(db.Open().IsInvalidArgument());
    CHECK(!std::filesystem::exists(path));
  }
  kv::DB empty(std::filesystem::path{});
  CHECK(empty.Open().IsInvalidArgument());
  kv::Options options; options.db_path = path; options.create_if_missing = false;
  kv::DB missing(options); CHECK(missing.Open().IsIOError());
  std::ofstream(path) << "regular file";
  kv::DB conflict(path); CHECK(conflict.Open().IsIOError());
  std::filesystem::remove(path);
  // A failed Open releases all state and may be retried on the same object.
  MustOK(conflict.Open()); MustOK(conflict.Close());
  options.create_if_missing = true; options.error_if_exists = true;
  kv::DB existing(options); CHECK(existing.Open().IsInvalidArgument());
  std::filesystem::remove_all(path);
  options.error_if_exists = false; options.block_cache_capacity = 0;
  kv::DB uncached(options); MustOK(uncached.Open()); MustOK(uncached.Put("k", "v"));
  MustOK(uncached.Close()); std::filesystem::remove_all(path);
}

void TestLifecycleAndDirectoryLock(const char* executable) {
  const auto path = TestDBPath("boundary_lifecycle");
  const auto alias = TestDBPath("boundary_alias");
  std::filesystem::remove_all(path); std::filesystem::remove(alias);
  kv::DB first(path), second(path);
  kv::WriteBatch empty;
  CHECK(first.Write(empty).IsIOError());
  CHECK(first.Compact().IsIOError());
  MustOK(first.Close()); MustOK(first.Open()); MustOK(first.Write(empty));
  CHECK(first.Open().IsInvalidArgument());
  CHECK(second.Open().IsIOError());
  std::filesystem::create_directory_symlink(path, alias);
  kv::DB aliased(alias); CHECK(aliased.Open().IsIOError());
  pid_t child = 0;
  std::string path_text = path.string();
  char* args[] = {const_cast<char*>(executable), const_cast<char*>("--lock-child"), path_text.data(), nullptr};
  CHECK(posix_spawn(&child, executable, nullptr, nullptr, args, environ) == 0);
  int result = 0; CHECK(waitpid(child, &result, 0) == child);
  CHECK(WIFEXITED(result) && WEXITSTATUS(result) == 0);
  const auto* zero = first.GetSnapshot(); CHECK(zero->sequence() == 0);
  MustOK(first.Put("k", "v"));
  std::string value;
  CHECK(first.Get("k", &value, kv::ReadOptions{zero}).IsNotFound());
  first.ReleaseSnapshot(zero); first.ReleaseSnapshot(zero); first.ReleaseSnapshot(nullptr);
  auto iterator = first.NewIterator(); iterator->SeekToFirst();
  MustOK(first.Close()); CHECK(first.Write(empty).IsIOError());
  // An existing iterator pins its read view; a new one is rejected after Close.
  CHECK(iterator->Valid() && iterator->value() == "v");
  CHECK(first.NewIterator()->status().IsIOError());
  MustOK(second.Open()); MustOK(second.Close());
  MustOK(first.Open()); MustOK(first.Close());
  std::atomic<bool> failed{false};
  auto cycles = [&] {
    for (int i = 0; i < 20; ++i) {
      auto status = first.Open();
      if (!status.ok() && !status.IsInvalidArgument()) failed = true;
      if (!first.Close().ok()) failed = true;
    }
  };
  std::thread a(cycles), b(cycles); a.join(); b.join(); CHECK(!failed);
  iterator.reset(); std::filesystem::remove(alias); std::filesystem::remove_all(path);
}

void TestWALSyncFailureDoesNotReuseSequences() {
  const auto path = TestDBPath("boundary_wal_sync"); std::filesystem::remove_all(path);
  bool fail = true;
  kv::Options options; options.db_path = path;
  options.testing_before_wal_sync = [&] {
    return fail ? kv::Status::IOError("injected WAL sync error") : kv::Status::OK();
  };
  kv::DB db(options); MustOK(db.Open());
  kv::WriteBatch batch; batch.Put("a", "one"); batch.Put("b", "two");
  CHECK(db.Write(batch).IsIOError());
  fail = false;
  CHECK(db.Put("c", "must not reuse sequence").IsIOError());
  CHECK(db.Write(kv::WriteBatch{}).IsIOError());
  CHECK(db.Close().IsIOError()); MustOK(db.Open());
  std::string value; MustOK(db.Get("a", &value)); CHECK(value == "one");
  MustOK(db.Get("b", &value)); CHECK(value == "two");
  CHECK(db.Get("c", &value).IsNotFound());
  CHECK(db.GetSnapshot()->sequence() == 2);
  MustOK(db.Put("c", "three")); CHECK(db.GetSnapshot()->sequence() == 3);
  MustOK(db.Close()); std::filesystem::remove_all(path);
}

void TestUncertainManifestPublishRetainsOutput() {
  const auto path = TestDBPath("boundary_manifest_sync"); std::filesystem::remove_all(path);
  bool fail = false;
  kv::Options options; options.db_path = path; options.level0_sstable_limit = 100;
  options.testing_before_manifest_sync = [&] {
    return fail ? kv::Status::IOError("injected MANIFEST sync error") : kv::Status::OK();
  };
  options.testing_before_compaction_publish = [&] { fail = true; };
  kv::DB db(options); MustOK(db.Open()); MustOK(db.Put("a", "one")); MustOK(db.Close());
  MustOK(db.Open()); MustOK(db.Put("b", "two")); MustOK(db.Close()); MustOK(db.Open());
  const auto before = ListSSTables(path).size();
  CHECK(db.Compact().IsIOError());
  CHECK(ListSSTables(path).size() == before + 1);
  CHECK(db.Put("c", "blocked").IsIOError()); CHECK(db.Close().IsIOError());
  fail = false; MustOK(db.Open());
  std::string value; MustOK(db.Get("a", &value)); CHECK(value == "one");
  MustOK(db.Get("b", &value)); CHECK(value == "two");
  CHECK(db.Get("c", &value).IsNotFound());
  MustOK(db.Close()); std::filesystem::remove_all(path);
}

void TestFileNumbersAndReopenCache() {
  std::uint64_t number = 0;
  CHECK(kv::ParseWALFileName(kv::WALFileName(1000000), &number) && number == 1000000);
  CHECK(kv::ParseSSTableFileName(kv::SSTableFileName(1000000), &number) && number == 1000000);
  CHECK(!kv::ParseWALFileName("wal_18446744073709551616.log", &number));
  CHECK(!kv::ParseWALFileName("wal_000000.log", &number));
  const auto path = TestDBPath("boundary_numbers"); std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  kv::Manifest manifest(path); kv::VersionEdit edit;
  edit.wal_file_number = 1000000; edit.next_file_number = 1000001; MustOK(manifest.Save(edit));
  kv::WALWriter wal(path / kv::WALFileName(1000000)); MustOK(wal.Open());
  kv::WriteBatch batch; batch.Put("k", "old"); MustOK(wal.AppendBatch(batch, 1)); wal.Close();
  MustOK(kv::SSTable::CreateFromEntries(path / kv::SSTableFileName(9000000), {{"orphan", 1, "ignored", false}}));
  kv::DB db(path); MustOK(db.Open());
  std::string value; CHECK(db.Get("orphan", &value).IsNotFound());
  MustOK(db.Put("k", "old")); MustOK(db.Close()); MustOK(db.Open());
  MustOK(db.Get("k", &value)); CHECK(value == "old");
  MustOK(db.Close()); std::filesystem::remove_all(path);
  // Reuse this object against a replacement directory with reused file numbers.
  { kv::DB replacement(path); MustOK(replacement.Open()); MustOK(replacement.Put("k", "new")); MustOK(replacement.Close()); }
  MustOK(db.Open()); MustOK(db.Get("k", &value)); CHECK(value == "new");
  MustOK(db.Close()); std::filesystem::remove_all(path);
}

void TestRecoverySequenceAndLengthBoundaries() {
  const auto path = TestDBPath("boundary_recovery");
  for (const auto sequence : {kv::SequenceNumber{0}, kv::SequenceNumber{1}, std::numeric_limits<kv::SequenceNumber>::max()}) {
    std::filesystem::remove_all(path); std::filesystem::create_directories(path);
    kv::WriteBatch batch; batch.Put("a", "one"); batch.Put("b", "two");
    kv::WALWriter wal(path / "wal_000001.log"); MustOK(wal.Open());
    if (sequence != 1) CHECK(wal.AppendBatch(batch, sequence).IsInvalidArgument());
    else { MustOK(wal.AppendBatch(batch, 1)); MustOK(wal.AppendBatch(batch, 2)); }
    wal.Close();
    if (sequence == 1) { kv::DB db(path); CHECK(db.Open().IsCorruption()); }
  }
  std::filesystem::remove_all(path); std::filesystem::create_directories(path);
  kv::Manifest manifest(path); kv::VersionEdit edit;
  edit.last_sequence = std::numeric_limits<kv::SequenceNumber>::max(); edit.next_file_number = 2;
  MustOK(manifest.Save(edit));
  { kv::DB db(path); MustOK(db.Open()); CHECK(db.Put("k", "v").IsInvalidArgument()); MustOK(db.Close()); }
  edit.last_sequence = 0; edit.next_file_number = std::numeric_limits<std::uint64_t>::max();
  MustOK(manifest.Save(edit));
  { kv::DB db(path); MustOK(db.Open()); CHECK(db.Put("k", "v").IsInvalidArgument()); MustOK(db.Close()); }
  // A huge declared tail length must be handled before allocating its payload.
  std::string header(25, '\0'); kv::EncodeFixed32(0x41574c4b, header.data()); header[4] = 1;
  kv::EncodeFixed64(1, header.data() + 5); kv::EncodeFixed32(1, header.data() + 13);
  kv::EncodeFixed32(std::numeric_limits<std::uint32_t>::max(), header.data() + 17);
  std::ofstream output(path / "wal_000001.log", std::ios::binary); output.write(header.data(), header.size()); output.close();
  kv::WALReader reader(path / "wal_000001.log"); kv::WALBatchRecord record; bool eof = false;
  MustOK(reader.ReadNext(&record, &eof)); CHECK(eof);
  CHECK(std::filesystem::file_size(path / "wal_000001.log") == 0);
  { std::ofstream tail(path / "MANIFEST", std::ios::app); tail << "record 18446744073709551615 0\n"; }
  MustOK(manifest.Load());
  std::filesystem::remove_all(path);
}

void TestMalformedMetadata() {
  const auto path = TestDBPath("boundary_metadata"); std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  for (const auto& fields : {std::string(""), std::string("next_file_number 0\nwal_file_number 1\nlast_sequence 0\n"),
       std::string("next_file_number -1\nwal_file_number 1\nlast_sequence 0\n"),
       std::string("next_file_number 2 extra\nwal_file_number 1\nlast_sequence 0\n"),
       std::string("next_file_number 2\nwal_file_number 1\nlast_sequence 0\nadd_file 1 ../sst_000001.data\n"),
       std::string("next_file_number 2\nwal_file_number 1\nlast_sequence 0\nadd_file 1 sst_000001.data -1 \"a\" \"z\" 0\n")}) {
    const std::string payload = "version_edit 1\n" + fields;
    std::ofstream out(path / "MANIFEST"); out << "record " << payload.size() << ' ' << kv::CRC32(payload) << '\n' << payload; out.close();
    kv::DB db(path); CHECK(db.Open().IsCorruption());
  }
  kv::BloomFilter filter; std::string bytes(8, '\0'); kv::EncodeFixed32(1, bytes.data() + 4);
  CHECK(kv::BloomFilter::Decode(bytes, &filter).IsCorruption());
  kv::EncodeFixed32(std::numeric_limits<std::uint32_t>::max(), bytes.data());
  CHECK(kv::BloomFilter::Decode(bytes, &filter).IsCorruption());
  const auto table_path = path / "sst_000001.data";
  MustOK(kv::SSTable::CreateFromEntries(table_path, {{"k", 1, "v", false}}));
  std::fstream table(table_path, std::ios::binary | std::ios::in | std::ios::out);
  table.seekg(-40, std::ios::end); char footer[40]; table.read(footer, 40);
  const auto index_offset = kv::DecodeFixed64(footer);
  char length[4]; kv::EncodeFixed32(std::numeric_limits<std::uint32_t>::max(), length);
  table.seekp(static_cast<std::streamoff>(index_offset + 8)); table.write(length, 4); table.close();
  std::shared_ptr<kv::SSTable> opened;
  CHECK(kv::SSTable::Open(1, table_path, nullptr, &opened).IsCorruption());
  std::filesystem::remove_all(path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--lock-child") {
    kv::DB db(argv[2]); return db.Open().IsIOError() ? 0 : 1;
  }
  TestOptionsAndPaths();
  TestLifecycleAndDirectoryLock(argv[0]);
  TestWALSyncFailureDoesNotReuseSequences();
  TestUncertainManifestPublishRetainsOutput();
  TestFileNumbersAndReopenCache();
  TestRecoverySequenceAndLengthBoundaries();
  TestMalformedMetadata();
  std::cout << "core_boundary_test passed\n";
}
