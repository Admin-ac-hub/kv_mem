#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "block_cache.h"
#include "manifest.h"
#include "memtable.h"
#include "sstable.h"
#include "status.h"
#include "types.h"
#include "wal.h"
#include "write_batch.h"

namespace kv {

class InternalIterator;

struct Options {
  std::filesystem::path db_path;
  size_t memtable_entries_limit = 1024;
  size_t level0_sstable_limit = 4;
  size_t block_size = 4096;
  size_t block_cache_capacity = 64;
  size_t bloom_bits_per_key = 10;
  bool create_if_missing = true;
  bool error_if_exists = false;
  bool testing_fail_flush = false;
  bool enable_event_log = false;
  std::function<void()> testing_after_read_version_pin = {};
};

struct DBStats {
  size_t sstable_count = 0;
  size_t level0_sstable_count = 0;
  size_t level1_sstable_count = 0;
  size_t level2_sstable_count = 0;
  std::uint64_t compaction_count = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t bloom_filtered = 0;
  std::uint64_t block_reads = 0;
  std::uint64_t sstable_full_scans = 0;
  std::uint64_t block_restart_seeks = 0;
};

class Snapshot {
 public:
  SequenceNumber sequence() const { return sequence_; }

 private:
  explicit Snapshot(SequenceNumber sequence) : sequence_(sequence) {}

  SequenceNumber sequence_ = 0;

  friend class DB;
};

struct ReadOptions {
  const Snapshot* snapshot = nullptr;
};

class Iterator {
 public:
  explicit Iterator(std::unique_ptr<InternalIterator> impl);
  ~Iterator();

  void SeekToFirst();
  void Seek(const std::string& target);
  void Next();
  bool Valid() const;
  const std::string& key() const;
  const std::string& value() const;
  Status status() const;

 private:
  std::unique_ptr<InternalIterator> impl_;
};

class DB {
 public:
  explicit DB(std::filesystem::path db_path);
  explicit DB(Options options);
  ~DB();

  Status Open();
  Status Close();
  Status Write(const WriteBatch& batch);
  Status Put(const std::string& key, const std::string& value);
  Status Get(const std::string& key, std::string* value);
  Status Get(const std::string& key, std::string* value, const ReadOptions& options);
  Status Delete(const std::string& key);
  Status Compact();
  DBStats Stats() const;
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot* snapshot);
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options = {});

 private:
  struct ImmutableMemTable {
    std::shared_ptr<MemTable> memtable;
    std::uint64_t wal_file_number = 0;
  };

  Status LoadOrRecoverManifest();
  Status OpenSSTables();
  Status Recover();
  Status MaybeFlushMemTable();
  Status FlushMemTable();
  void BackgroundWorkerLoop();
  Status FlushImmutableMemTable(const ImmutableMemTable& immutable,
                                std::uint64_t sstable_number,
                                std::shared_ptr<SSTable>* table,
                                SSTableMeta* meta);
  Status RemoveObsoleteWALFiles(std::uint64_t oldest_live_wal);
  void SetBackgroundErrorLocked(Status status);
  Status MaybeCompact();
  Status CompactUnlocked();
  SequenceNumber ReadSequence(const ReadOptions& options) const;
  SequenceNumber MinActiveSnapshotSequence() const;
  Status SaveManifest();
  void LogEvent(const std::string& message) const;
  std::filesystem::path SSTablePath(std::uint64_t number) const;
  std::filesystem::path WALPath(std::uint64_t number) const;

  Options options_;
  std::unique_ptr<WALWriter> wal_;
  std::uint64_t active_wal_file_number_ = 1;
  std::uint64_t active_memtable_oldest_wal_file_number_ = 1;
  std::shared_ptr<MemTable> active_memtable_;
  std::deque<ImmutableMemTable> immutable_memtables_;
  std::vector<std::shared_ptr<SSTable>> sstables_;
  std::unique_ptr<Manifest> manifest_;
  std::shared_ptr<BlockCache> block_cache_;
  std::vector<std::unique_ptr<Snapshot>> snapshots_;
  std::uint64_t compaction_count_ = 0;
  std::uint64_t sstable_full_scans_ = 0;
  std::thread background_worker_;
  std::condition_variable background_cv_;
  bool background_stop_ = false;
  bool background_work_pending_ = false;
  bool closed_ = true;
  Status background_status_;
  mutable std::mutex write_mu_;
  mutable std::mutex version_mu_;
  mutable std::shared_mutex memtable_mu_;
  mutable std::mutex bg_mu_;
};

}  // namespace kv
