#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "status.h"
#include "types.h"

namespace kv {

struct SSTableMeta {
  std::uint64_t file_number = 0;
  std::filesystem::path file_path;
  int level = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t file_size = 0;
};

struct VersionEdit {
  std::uint64_t next_file_number = 1;
  std::uint64_t wal_file_number = 1;
  SequenceNumber last_sequence = 0;
  std::vector<SSTableMeta> sstables;
};

class Manifest {
 public:
  explicit Manifest(std::filesystem::path db_path);

  Status Load();
  Status Save(const VersionEdit& edit);
  bool Exists() const;

  const std::vector<SSTableMeta>& SSTables() const;
  std::uint64_t NextFileNumber() const;
  std::uint64_t WALFileNumber() const;
  SequenceNumber LastSequence() const;
  std::uint64_t AllocateFileNumber();
  void SetNextFileNumber(std::uint64_t number);
  void SetWALFileNumber(std::uint64_t number);
  void SetLastSequence(SequenceNumber sequence);
  void SetSSTables(std::vector<SSTableMeta> sstables);
  void SetCurrentEdit(VersionEdit edit);
  VersionEdit CurrentEdit() const;

 private:
  Status LoadLegacyFullManifest(const std::string& contents);
  Status LoadEditLog();
  Status WriteCurrentFile();

  std::filesystem::path db_path_;
  std::filesystem::path manifest_path_;
  VersionEdit edit_;
  VersionEdit persisted_edit_;
  bool legacy_format_loaded_ = false;
};

}  // namespace kv
