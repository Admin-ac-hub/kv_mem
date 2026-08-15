#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "block_cache.h"
#include "bloom_filter.h"
#include "internal_iterator.h"
#include "status.h"
#include "types.h"

namespace kv {

class SSTable : public std::enable_shared_from_this<SSTable> {
 public:
  enum class ValueType : std::uint8_t {
    kPut = 1,
    kDelete = 2,
  };

  static Status CreateFromEntries(const std::filesystem::path& path,
                                  const std::vector<VersionedEntry>& entries,
                                  size_t block_size = 4096,
                                  size_t bloom_bits_per_key = 10);
  static Status Open(std::uint64_t file_number,
                     const std::filesystem::path& path,
                     std::shared_ptr<BlockCache> block_cache,
                     std::shared_ptr<SSTable>* table);
  ~SSTable();

  Status Get(const std::string& key,
             SequenceNumber read_sequence,
             std::optional<std::string>* value) const;
  Status Get(const std::string& key,
             SequenceNumber read_sequence,
             std::string* value,
             bool* found) const;
  Status Entries(std::vector<VersionedEntry>* entries) const;
  std::unique_ptr<InternalIterator> NewIterator(SequenceNumber read_sequence) const;
  std::uint64_t FileNumber() const;
  const std::filesystem::path& FilePath() const;
  std::uint64_t BloomFilteredCount() const;
  std::uint64_t BlockReadCount() const;
  std::uint64_t RestartSeekCount() const;

 private:
  struct IndexEntry {
    std::string last_internal_key;
    std::uint64_t block_offset = 0;
    std::uint64_t block_size = 0;
  };

  friend class SSTableInternalIterator;

  SSTable(std::uint64_t file_number, std::filesystem::path path);

  Status LoadIndex();
  Status ReadRawBlock(const IndexEntry& index, std::string* block_data) const;
  Status ReadBlock(const IndexEntry& index, std::vector<VersionedEntry>* entries) const;
  Status GetFromBlock(const IndexEntry& index,
                      const std::string& key,
                      SequenceNumber read_sequence,
                      std::optional<std::string>* value,
                      bool* found) const;
  Status DecodeBlock(const std::string& block_data,
                     std::vector<VersionedEntry>* entries) const;

  std::uint64_t file_number_;
  std::filesystem::path path_;
  int fd_ = -1;
  std::vector<IndexEntry> index_;
  BloomFilter filter_;
  std::shared_ptr<BlockCache> block_cache_;
  mutable std::atomic<std::uint64_t> bloom_filtered_count_{0};
  mutable std::atomic<std::uint64_t> block_read_count_{0};
  mutable std::atomic<std::uint64_t> restart_seek_count_{0};
};

}  // namespace kv
