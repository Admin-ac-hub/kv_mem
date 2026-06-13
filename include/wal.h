#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "status.h"
#include "types.h"
#include "write_batch.h"

namespace kv {

enum class RecordType : std::uint8_t {
  kBatch = 1,
};

struct WALBatchRecord {
  SequenceNumber sequence = 0;
  WriteBatch batch;
};

class WALWriter {
 public:
  explicit WALWriter(std::filesystem::path path);
  ~WALWriter();

  WALWriter(const WALWriter&) = delete;
  WALWriter& operator=(const WALWriter&) = delete;

  Status Open();
  void Close();
  Status Reset();
  Status AppendBatch(const WriteBatch& batch, SequenceNumber sequence);
  Status AppendPut(const std::string& key, SequenceNumber sequence, const std::string& value);
  Status AppendDelete(const std::string& key, SequenceNumber sequence);
  const std::filesystem::path& Path() const;

 private:
  std::filesystem::path path_;
  std::ofstream stream_;
};

class WALReader {
 public:
  explicit WALReader(std::filesystem::path path);

  Status ReadNext(WALBatchRecord* record, bool* eof);

 private:
  Status ReadLegacyRecord(std::uint32_t expected_checksum, WALBatchRecord* record);
  Status ReadBatchRecord(WALBatchRecord* record, bool* eof);

  bool opened_ = false;
  std::filesystem::path path_;
  std::ifstream stream_;
};

}  // namespace kv
