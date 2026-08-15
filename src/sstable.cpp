#include "sstable.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "format.h"

namespace kv {

namespace {

constexpr std::uint64_t kSSTableMagic = 0x4b565354424c4b33ULL;  // KVSTBLK3
constexpr size_t kFooterSize = 40;
constexpr size_t kRecordHeaderSize = 17;
constexpr std::uint32_t kDataBlockMagic = 0x344b4c42;  // BLK4
constexpr std::uint32_t kRestartInterval = 16;
constexpr size_t kBlockTrailerSize = 5;
constexpr std::uint8_t kNoCompression = 0;
constexpr std::uint8_t kLegacyRawCompression = 1;

void AppendFixed32(std::string* out, std::uint32_t value) {
  const size_t old_size = out->size();
  out->resize(old_size + 4);
  EncodeFixed32(value, out->data() + old_size);
}

void AppendFixed64(std::string* out, std::uint64_t value) {
  const size_t old_size = out->size();
  out->resize(old_size + 8);
  EncodeFixed64(value, out->data() + old_size);
}

Status WriteString(std::ofstream* stream, const std::string& data, const std::string& what) {
  stream->write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!*stream) {
    return Status::IOError("failed to write " + what);
  }
  return Status::OK();
}

Status ReadExactAt(int fd,
                   std::uint64_t offset,
                   char* data,
                   size_t size,
                   const std::filesystem::path& path) {
  const auto max_offset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (offset > max_offset || (size > 0 && size - 1 > max_offset - offset)) {
    return Status::Corruption("SSTable read range is too large: " + path.string());
  }

  size_t completed = 0;
  while (completed < size) {
    const size_t remaining = size - completed;
    const size_t chunk = std::min(
        remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t read_size =
        ::pread(fd, data + completed, chunk, static_cast<off_t>(offset + completed));
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IOError("failed to read SSTable " + path.string() + ": " +
                             std::strerror(errno));
    }
    if (read_size == 0) {
      return Status::Corruption("incomplete SSTable read: " + path.string());
    }
    completed += static_cast<size_t>(read_size);
  }
  return Status::OK();
}

std::string EncodeBlock(const std::vector<VersionedEntry>& entries,
                        size_t begin,
                        size_t end) {
  std::string payload;
  AppendFixed32(&payload, kDataBlockMagic);
  AppendFixed32(&payload, kRestartInterval);
  AppendFixed64(&payload, static_cast<std::uint64_t>(end - begin));
  std::vector<std::uint32_t> restart_offsets;
  std::string previous_key;
  for (size_t i = begin; i < end; ++i) {
    const auto& entry = entries[i];
    const bool restart = ((i - begin) % kRestartInterval) == 0;
    if (restart) {
      restart_offsets.push_back(static_cast<std::uint32_t>(payload.size()));
    }
    size_t shared = 0;
    if (!restart) {
      const size_t limit = std::min(previous_key.size(), entry.key.size());
      while (shared < limit && previous_key[shared] == entry.key[shared]) {
        ++shared;
      }
    }
    const std::string_view unshared(entry.key.data() + shared, entry.key.size() - shared);
    AppendFixed32(&payload, static_cast<std::uint32_t>(shared));
    AppendFixed32(&payload, static_cast<std::uint32_t>(unshared.size()));
    AppendFixed32(&payload, static_cast<std::uint32_t>(entry.value.size()));
    payload.push_back(static_cast<char>(entry.deleted ? SSTable::ValueType::kDelete
                                                      : SSTable::ValueType::kPut));
    AppendFixed64(&payload, entry.sequence);
    payload.append(unshared.data(), unshared.size());
    payload.append(entry.value);
    previous_key = entry.key;
  }
  for (const auto offset : restart_offsets) {
    AppendFixed32(&payload, offset);
  }
  AppendFixed32(&payload, static_cast<std::uint32_t>(restart_offsets.size()));

  payload.push_back(static_cast<char>(kNoCompression));
  const std::uint32_t checksum = CRC32(payload);
  AppendFixed32(&payload, checksum);
  return payload;
}

Status DecodeLegacyBlockPayload(std::string_view payload,
                                std::vector<VersionedEntry>* entries) {
  if (payload.size() < 8) {
    return Status::Corruption("invalid SSTable data block");
  }
  const char* ptr = payload.data();
  const char* end = ptr + payload.size();
  const std::uint64_t record_count = DecodeFixed64(ptr);
  ptr += 8;
  if (record_count > (payload.size() - 8) / kRecordHeaderSize) {
    return Status::Corruption("invalid SSTable data record count");
  }
  entries->clear();
  entries->reserve(static_cast<size_t>(record_count));

  for (std::uint64_t i = 0; i < record_count; ++i) {
    if (end - ptr < static_cast<std::ptrdiff_t>(kRecordHeaderSize)) {
      return Status::Corruption("invalid SSTable data record");
    }
    const auto type = static_cast<SSTable::ValueType>(static_cast<std::uint8_t>(ptr[0]));
    const SequenceNumber sequence = DecodeFixed64(ptr + 1);
    const std::uint32_t key_size = DecodeFixed32(ptr + 9);
    const std::uint32_t value_size = DecodeFixed32(ptr + 13);
    ptr += kRecordHeaderSize;
    const size_t data_size = static_cast<size_t>(key_size) + value_size;
    if (static_cast<size_t>(end - ptr) < data_size) {
      return Status::Corruption("invalid SSTable data payload");
    }
    VersionedEntry entry;
    entry.key.assign(ptr, key_size);
    entry.sequence = sequence;
    ptr += key_size;
    entry.value.assign(ptr, value_size);
    ptr += value_size;

    switch (type) {
      case SSTable::ValueType::kPut:
        entry.deleted = false;
        break;
      case SSTable::ValueType::kDelete:
        entry.value.clear();
        entry.deleted = true;
        break;
      default:
        return Status::Corruption("unknown SSTable record type");
    }
    entries->push_back(std::move(entry));
  }
  if (ptr != end) {
    return Status::Corruption("trailing SSTable data block bytes");
  }
  return Status::OK();
}

Status DecodeBlockPayload(std::string_view block_data,
                          std::string_view* payload,
                          bool* prefix_compressed) {
  *prefix_compressed = false;
  if (block_data.size() >= kBlockTrailerSize + 16 &&
      DecodeFixed32(block_data.data()) == kDataBlockMagic) {
    const auto compression_type = static_cast<std::uint8_t>(
        block_data[block_data.size() - kBlockTrailerSize]);
    if (compression_type != kNoCompression &&
        compression_type != kLegacyRawCompression) {
      return Status::Corruption("unknown SSTable compression type");
    }
    const std::uint32_t expected_crc =
        DecodeFixed32(block_data.data() + block_data.size() - 4);
    const std::string_view checksummed(block_data.data(), block_data.size() - 4);
    if (CRC32(checksummed) != expected_crc) {
      return Status::Corruption("SSTable data block checksum mismatch");
    }
    *payload = block_data.substr(0, block_data.size() - kBlockTrailerSize);
    *prefix_compressed = true;
    return Status::OK();
  }

  if (block_data.size() < 12) {
    return Status::Corruption("invalid SSTable data block");
  }
  const std::uint32_t expected_crc = DecodeFixed32(block_data.data() + block_data.size() - 4);
  const std::string_view legacy_payload(block_data.data(), block_data.size() - 4);
  if (CRC32(legacy_payload) != expected_crc) {
    return Status::Corruption("SSTable data block checksum mismatch");
  }
  *payload = legacy_payload;
  return Status::OK();
}

struct PrefixBlockLayout {
  std::string_view payload;
  const char* records_begin = nullptr;
  const char* records_end = nullptr;
  const char* restart_base = nullptr;
  std::uint32_t restart_count = 0;
  std::uint64_t record_count = 0;
};

Status ParsePrefixBlockLayout(std::string_view payload, PrefixBlockLayout* layout) {
  if (payload.size() < 20 || DecodeFixed32(payload.data()) != kDataBlockMagic) {
    return Status::Corruption("invalid prefix-compressed SSTable block");
  }
  const char* begin = payload.data();
  const char* end = begin + payload.size();
  const std::uint32_t restart_interval = DecodeFixed32(begin + 4);
  const std::uint64_t record_count = DecodeFixed64(begin + 8);
  if (restart_interval == 0 || record_count == 0) {
    return Status::Corruption("invalid prefix-compressed SSTable restart interval");
  }
  const std::uint32_t restart_count = DecodeFixed32(end - 4);
  const size_t restart_bytes = static_cast<size_t>(restart_count) * 4 + 4;
  const std::uint64_t expected_restart_count =
      record_count / restart_interval + (record_count % restart_interval != 0);
  if (restart_count == 0 || restart_count != expected_restart_count ||
      payload.size() < 16 + restart_bytes) {
    return Status::Corruption("invalid prefix-compressed SSTable restart area");
  }

  const char* records_end = end - restart_bytes;
  const size_t records_size = static_cast<size_t>(records_end - (begin + 16));
  if (record_count > records_size / 21) {
    return Status::Corruption("invalid prefix-compressed SSTable record count");
  }
  std::uint32_t previous_offset = 0;
  for (std::uint32_t i = 0; i < restart_count; ++i) {
    const std::uint32_t offset = DecodeFixed32(records_end + i * 4);
    if (offset < 16 || static_cast<size_t>(offset - 16) > records_size - 21 ||
        (i == 0 && offset != 16) || (i > 0 && offset <= previous_offset) ||
        DecodeFixed32(begin + offset) != 0) {
      return Status::Corruption("invalid prefix-compressed SSTable restart offset");
    }
    previous_offset = offset;
  }

  layout->payload = payload;
  layout->records_begin = begin + 16;
  layout->records_end = records_end;
  layout->restart_base = layout->records_end;
  layout->restart_count = restart_count;
  layout->record_count = record_count;
  return Status::OK();
}

Status DecodePrefixRecordAt(const PrefixBlockLayout& layout,
                            const char* ptr,
                            const std::string& previous_key,
                            VersionedEntry* entry,
                            const char** next) {
  if (ptr < layout.records_begin || ptr > layout.records_end ||
      layout.records_end - ptr < 21) {
    return Status::Corruption("invalid prefix-compressed SSTable record");
  }
  const std::uint32_t shared = DecodeFixed32(ptr);
  const std::uint32_t unshared = DecodeFixed32(ptr + 4);
  const std::uint32_t value_size = DecodeFixed32(ptr + 8);
  const auto type = static_cast<SSTable::ValueType>(static_cast<std::uint8_t>(ptr[12]));
  const SequenceNumber sequence = DecodeFixed64(ptr + 13);
  ptr += 21;
  const size_t data_size = static_cast<size_t>(unshared) + value_size;
  if (shared > previous_key.size() ||
      static_cast<size_t>(layout.records_end - ptr) < data_size) {
    return Status::Corruption("invalid prefix-compressed SSTable payload");
  }

  entry->key.assign(previous_key.data(), shared);
  entry->key.append(ptr, unshared);
  ptr += unshared;
  entry->sequence = sequence;
  entry->value.assign(ptr, value_size);
  ptr += value_size;

  switch (type) {
    case SSTable::ValueType::kPut:
      entry->deleted = false;
      break;
    case SSTable::ValueType::kDelete:
      entry->value.clear();
      entry->deleted = true;
      break;
    default:
      return Status::Corruption("unknown SSTable record type");
  }
  *next = ptr;
  return Status::OK();
}

Status DecodePrefixCompressedPayload(std::string_view payload,
                                     std::vector<VersionedEntry>* entries) {
  PrefixBlockLayout layout;
  Status status = ParsePrefixBlockLayout(payload, &layout);
  if (!status.ok()) {
    return status;
  }

  entries->clear();
  entries->reserve(static_cast<size_t>(layout.record_count));
  const char* ptr = layout.records_begin;
  std::string previous_key;
  for (std::uint64_t i = 0; i < layout.record_count; ++i) {
    VersionedEntry entry;
    const char* next = nullptr;
    status = DecodePrefixRecordAt(layout, ptr, previous_key, &entry, &next);
    if (!status.ok()) {
      return status;
    }
    previous_key = entry.key;
    ptr = next;
    entries->push_back(std::move(entry));
  }
  if (ptr != layout.records_end) {
    return Status::Corruption("trailing prefix-compressed SSTable record bytes");
  }
  return Status::OK();
}

Status RestartKey(const PrefixBlockLayout& layout,
                  std::uint32_t restart_index,
                  std::string* key) {
  if (restart_index >= layout.restart_count) {
    return Status::Corruption("invalid prefix-compressed SSTable restart index");
  }
  const std::uint32_t offset =
      DecodeFixed32(layout.restart_base + restart_index * 4);
  const char* ptr = layout.payload.data() + offset;
  VersionedEntry entry;
  const char* next = nullptr;
  Status status = DecodePrefixRecordAt(layout, ptr, "", &entry, &next);
  if (!status.ok()) {
    return status;
  }
  *key = EncodeInternalKey(entry.key, entry.sequence);
  return Status::OK();
}

}  // namespace

class SSTableInternalIterator : public InternalIterator {
 public:
  SSTableInternalIterator(std::shared_ptr<const SSTable> table,
                          SequenceNumber read_sequence)
      : table_(std::move(table)), read_sequence_(read_sequence) {}

  void SeekToFirst() override {
    status_ = Status::OK();
    block_index_ = 0;
    entry_index_ = 0;
    block_entries_.clear();
    loaded_block_ = false;
    SkipUntilVisible("");
  }

  void Seek(const std::string& target) override {
    status_ = Status::OK();
    block_entries_.clear();
    loaded_block_ = false;
    entry_index_ = 0;

    const std::string internal_key = EncodeInternalKey(target, read_sequence_);
    auto it = std::lower_bound(
        table_->index_.begin(), table_->index_.end(), internal_key,
        [](const SSTable::IndexEntry& entry, const std::string& target_key) {
          return entry.last_internal_key < target_key;
        });
    block_index_ = static_cast<size_t>(it - table_->index_.begin());
    SkipUntilVisible(target);
  }

  void Next() override {
    if (!Valid()) {
      return;
    }
    AdvanceRaw();
    SkipUntilVisible("");
  }

  bool Valid() const override {
    return status_.ok() && loaded_block_ && entry_index_ < block_entries_.size();
  }

  const std::string& key() const override {
    static const std::string empty;
    return Valid() ? block_entries_[entry_index_].key : empty;
  }

  const std::string& value() const override {
    static const std::string empty;
    return Valid() ? block_entries_[entry_index_].value : empty;
  }

  SequenceNumber sequence() const override {
    return Valid() ? block_entries_[entry_index_].sequence : 0;
  }

  bool deleted() const override {
    return Valid() && block_entries_[entry_index_].deleted;
  }

  Status status() const override {
    return status_;
  }

 private:
  bool EnsureBlockLoaded() {
    while (!loaded_block_) {
      if (block_index_ >= table_->index_.size()) {
        return false;
      }
      block_entries_.clear();
      status_ = table_->ReadBlock(table_->index_[block_index_], &block_entries_);
      if (!status_.ok()) {
        return false;
      }
      loaded_block_ = true;
      entry_index_ = 0;
      if (block_entries_.empty()) {
        loaded_block_ = false;
        ++block_index_;
      }
    }
    return true;
  }

  void AdvanceRaw() {
    if (!loaded_block_) {
      return;
    }
    ++entry_index_;
    if (entry_index_ >= block_entries_.size()) {
      loaded_block_ = false;
      block_entries_.clear();
      ++block_index_;
      entry_index_ = 0;
    }
  }

  void SkipUntilVisible(const std::string& target) {
    while (EnsureBlockLoaded()) {
      const VersionedEntry& entry = block_entries_[entry_index_];
      if (!target.empty() && entry.key < target) {
        AdvanceRaw();
        continue;
      }
      if (entry.sequence > read_sequence_) {
        AdvanceRaw();
        continue;
      }
      return;
    }
  }

  std::shared_ptr<const SSTable> table_;
  SequenceNumber read_sequence_ = 0;
  size_t block_index_ = 0;
  size_t entry_index_ = 0;
  bool loaded_block_ = false;
  std::vector<VersionedEntry> block_entries_;
  Status status_;
};

SSTable::SSTable(std::uint64_t file_number, std::filesystem::path path)
    : file_number_(file_number), path_(std::move(path)) {}

SSTable::~SSTable() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Status SSTable::CreateFromEntries(
    const std::filesystem::path& path,
    const std::vector<VersionedEntry>& entries,
    size_t block_size,
    size_t bloom_bits_per_key) {
  std::filesystem::create_directories(path.parent_path());

  const std::filesystem::path tmp_path = path.string() + ".tmp";
  std::vector<IndexEntry> index;
  std::set<std::string> unique_keys;
  for (const auto& entry : entries) {
    unique_keys.insert(entry.key);
  }
  std::vector<std::string> keys(unique_keys.begin(), unique_keys.end());

  {
    std::ofstream stream(tmp_path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      return Status::IOError("failed to open SSTable temp file: " + tmp_path.string());
    }

    size_t begin = 0;
    while (begin < entries.size()) {
      size_t end = begin;
      size_t approx_size = 8;
      do {
        approx_size += kRecordHeaderSize + entries[end].key.size() + entries[end].value.size();
        ++end;
      } while (end < entries.size() && approx_size < block_size);
      while (end < entries.size() && entries[end].key == entries[end - 1].key) {
        approx_size += kRecordHeaderSize + entries[end].key.size() + entries[end].value.size();
        ++end;
      }

      const auto block_offset = static_cast<std::uint64_t>(stream.tellp());
      std::string block = EncodeBlock(entries, begin, end);
      Status status = WriteString(&stream, block, "SSTable data block");
      if (!status.ok()) {
        return status;
      }
      index.push_back(
          IndexEntry{EncodeInternalKey(entries[end - 1].key,
                                       entries[end - 1].sequence),
                     block_offset,
                     static_cast<std::uint64_t>(block.size())});
      begin = end;
    }

    const auto index_offset = static_cast<std::uint64_t>(stream.tellp());
    std::string index_block;
    AppendFixed64(&index_block, static_cast<std::uint64_t>(index.size()));
    for (const auto& item : index) {
      AppendFixed32(&index_block, static_cast<std::uint32_t>(item.last_internal_key.size()));
      index_block.append(item.last_internal_key);
      AppendFixed64(&index_block, item.block_offset);
      AppendFixed64(&index_block, item.block_size);
    }
    Status status = WriteString(&stream, index_block, "SSTable index block");
    if (!status.ok()) {
      return status;
    }

    const auto filter_offset = static_cast<std::uint64_t>(stream.tellp());
    const std::string filter_block = BloomFilter::Build(keys, bloom_bits_per_key).Encode();
    status = WriteString(&stream, filter_block, "SSTable filter block");
    if (!status.ok()) {
      return status;
    }

    std::string footer;
    AppendFixed64(&footer, index_offset);
    AppendFixed64(&footer, static_cast<std::uint64_t>(index_block.size()));
    AppendFixed64(&footer, filter_offset);
    AppendFixed64(&footer, static_cast<std::uint64_t>(filter_block.size()));
    AppendFixed64(&footer, kSSTableMagic);
    status = WriteString(&stream, footer, "SSTable footer");
    if (!status.ok()) {
      return status;
    }

    stream.flush();
    if (!stream) {
      return Status::IOError("failed to flush SSTable temp file: " + tmp_path.string());
    }
  }

  Status status = FsyncFile(tmp_path);
  if (!status.ok()) {
    std::filesystem::remove(tmp_path);
    return status;
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path);
    return Status::IOError("failed to rename SSTable temp file: " + ec.message());
  }

  status = FsyncDirectory(path.parent_path());
  if (!status.ok()) {
    return status;
  }

  return Status::OK();
}

Status SSTable::Open(std::uint64_t file_number,
                     const std::filesystem::path& path,
                     std::shared_ptr<BlockCache> block_cache,
                     std::shared_ptr<SSTable>* table) {
  auto candidate = std::shared_ptr<SSTable>(new SSTable(file_number, path));
  candidate->block_cache_ = std::move(block_cache);
  candidate->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (candidate->fd_ < 0) {
    return Status::IOError("failed to open SSTable " + path.string() + ": " +
                           std::strerror(errno));
  }
  Status status = candidate->LoadIndex();
  if (!status.ok()) {
    return status;
  }
  *table = std::move(candidate);
  return Status::OK();
}

Status SSTable::Get(const std::string& key,
                    SequenceNumber read_sequence,
                    std::optional<std::string>* value) const {
  value->reset();
  if (!filter_.MayContain(key)) {
    bloom_filtered_count_.fetch_add(1, std::memory_order_relaxed);
    return Status::NotFound("key not found");
  }

  const std::string target = EncodeInternalKey(key, read_sequence);
  auto it = std::lower_bound(index_.begin(), index_.end(), target,
                             [](const IndexEntry& entry, const std::string& target) {
                               return entry.last_internal_key < target;
                             });
  if (it == index_.end()) {
    return Status::NotFound("key not found");
  }

  bool found = false;
  Status status = GetFromBlock(*it, key, read_sequence, value, &found);
  if (!status.ok()) {
    return status;
  }
  if (found) {
    return Status::OK();
  }
  return Status::NotFound("key not found");
}

Status SSTable::Get(const std::string& key,
                    SequenceNumber read_sequence,
                    std::string* value,
                    bool* found) const {
  std::optional<std::string> maybe_value;
  Status status = Get(key, read_sequence, &maybe_value);
  if (!status.ok()) {
    *found = false;
    return status;
  }
  *found = true;
  if (!maybe_value.has_value()) {
    return Status::NotFound("key deleted");
  }
  *value = *maybe_value;
  return Status::OK();
}

Status SSTable::Entries(std::vector<VersionedEntry>* entries) const {
  entries->clear();
  for (const auto& item : index_) {
    std::vector<VersionedEntry> block_entries;
    Status status = ReadBlock(item, &block_entries);
    if (!status.ok()) {
      return status;
    }
    entries->insert(entries->end(),
                    std::make_move_iterator(block_entries.begin()),
                    std::make_move_iterator(block_entries.end()));
  }
  return Status::OK();
}

std::unique_ptr<InternalIterator> SSTable::NewIterator(SequenceNumber read_sequence) const {
  return std::make_unique<SSTableInternalIterator>(shared_from_this(), read_sequence);
}

std::uint64_t SSTable::FileNumber() const {
  return file_number_;
}

const std::filesystem::path& SSTable::FilePath() const {
  return path_;
}

std::uint64_t SSTable::BloomFilteredCount() const {
  return bloom_filtered_count_.load(std::memory_order_relaxed);
}

std::uint64_t SSTable::BlockReadCount() const {
  return block_read_count_.load(std::memory_order_relaxed);
}

std::uint64_t SSTable::RestartSeekCount() const {
  return restart_seek_count_.load(std::memory_order_relaxed);
}

Status SSTable::LoadIndex() {
  struct stat file_info {};
  if (::fstat(fd_, &file_info) != 0) {
    return Status::IOError("failed to stat SSTable " + path_.string() + ": " +
                           std::strerror(errno));
  }
  if (file_info.st_size < 0) {
    return Status::Corruption("invalid SSTable size: " + path_.string());
  }
  const auto file_size = static_cast<std::uint64_t>(file_info.st_size);
  if (file_size < kFooterSize) {
    return Status::Corruption("SSTable too small: " + path_.string());
  }

  std::array<char, kFooterSize> footer{};
  Status status = ReadExactAt(fd_, file_size - kFooterSize, footer.data(), footer.size(), path_);
  if (!status.ok()) {
    return status;
  }

  const std::uint64_t index_offset = DecodeFixed64(footer.data());
  const std::uint64_t index_size = DecodeFixed64(footer.data() + 8);
  const std::uint64_t filter_offset = DecodeFixed64(footer.data() + 16);
  const std::uint64_t filter_size = DecodeFixed64(footer.data() + 24);
  const std::uint64_t magic = DecodeFixed64(footer.data() + 32);
  if (magic != kSSTableMagic) {
    return Status::Corruption("invalid SSTable magic: " + path_.string());
  }
  const std::uint64_t data_end = file_size - kFooterSize;
  if (index_offset > filter_offset || index_size != filter_offset - index_offset ||
      filter_offset > data_end || filter_size != data_end - filter_offset ||
      index_size > std::numeric_limits<size_t>::max() ||
      filter_size > std::numeric_limits<size_t>::max()) {
    return Status::Corruption("invalid SSTable footer: " + path_.string());
  }

  std::string index_block(static_cast<size_t>(index_size), '\0');
  status = ReadExactAt(fd_, index_offset, index_block.data(), index_block.size(), path_);
  if (!status.ok()) {
    return status;
  }

  const char* ptr = index_block.data();
  const char* end = ptr + index_block.size();
  if (end - ptr < 8) {
    return Status::Corruption("invalid SSTable index");
  }
  const std::uint64_t index_count = DecodeFixed64(ptr);
  ptr += 8;
  index_.clear();
  for (std::uint64_t i = 0; i < index_count; ++i) {
    if (end - ptr < 4) {
      return Status::Corruption("invalid SSTable index key size");
    }
    const std::uint32_t key_size = DecodeFixed32(ptr);
    ptr += 4;
    if (end - ptr < static_cast<std::ptrdiff_t>(key_size + 16)) {
      return Status::Corruption("invalid SSTable index entry");
    }
    std::string last_internal_key(ptr, key_size);
    ptr += key_size;
    const std::uint64_t block_offset = DecodeFixed64(ptr);
    ptr += 8;
    const std::uint64_t block_size = DecodeFixed64(ptr);
    ptr += 8;
    if (block_offset > index_offset || block_size > index_offset - block_offset ||
        block_size > std::numeric_limits<size_t>::max()) {
      return Status::Corruption("invalid SSTable data block range");
    }
    index_.push_back(IndexEntry{std::move(last_internal_key), block_offset, block_size});
  }
  if (ptr != end) {
    return Status::Corruption("trailing SSTable index bytes");
  }

  std::string filter_block(static_cast<size_t>(filter_size), '\0');
  status = ReadExactAt(fd_, filter_offset, filter_block.data(), filter_block.size(), path_);
  if (!status.ok()) {
    return status;
  }
  return BloomFilter::Decode(filter_block, &filter_);
}

Status SSTable::ReadRawBlock(const IndexEntry& index,
                             std::string* block_data) const {
  if (block_cache_ != nullptr &&
      block_cache_->Get(file_number_, index.block_offset, block_data)) {
    return Status::OK();
  }

  block_data->assign(static_cast<size_t>(index.block_size), '\0');
  Status status =
      ReadExactAt(fd_, index.block_offset, block_data->data(), block_data->size(), path_);
  if (!status.ok()) {
    return status;
  }
  block_read_count_.fetch_add(1, std::memory_order_relaxed);
  if (block_cache_ != nullptr) {
    block_cache_->Put(file_number_, index.block_offset, *block_data);
  }
  return Status::OK();
}

Status SSTable::ReadBlock(const IndexEntry& index,
                          std::vector<VersionedEntry>* entries) const {
  std::string block_data;
  Status status = ReadRawBlock(index, &block_data);
  if (!status.ok()) {
    return status;
  }
  status = DecodeBlock(block_data, entries);
  if (!status.ok()) {
    return status;
  }
  return Status::OK();
}

Status SSTable::GetFromBlock(const IndexEntry& index,
                             const std::string& key,
                             SequenceNumber read_sequence,
                             std::optional<std::string>* value,
                             bool* found) const {
  *found = false;
  std::string block_data;
  Status status = ReadRawBlock(index, &block_data);
  if (!status.ok()) {
    return status;
  }

  std::string_view payload;
  bool prefix_compressed = false;
  status = DecodeBlockPayload(block_data, &payload, &prefix_compressed);
  if (!status.ok()) {
    return status;
  }
  if (!prefix_compressed) {
    std::vector<VersionedEntry> entries;
    status = DecodeLegacyBlockPayload(payload, &entries);
    if (!status.ok()) {
      return status;
    }
    for (const auto& entry : entries) {
      if (entry.key == key && entry.sequence <= read_sequence) {
        *found = true;
        if (!entry.deleted) {
          *value = entry.value;
        }
        return Status::OK();
      }
      if (entry.key > key) {
        break;
      }
    }
    return Status::OK();
  }

  PrefixBlockLayout layout;
  status = ParsePrefixBlockLayout(payload, &layout);
  if (!status.ok()) {
    return status;
  }
  restart_seek_count_.fetch_add(1, std::memory_order_relaxed);

  const std::string target = EncodeInternalKey(key, read_sequence);
  std::uint32_t left = 0;
  std::uint32_t right = layout.restart_count;
  while (left < right) {
    const std::uint32_t mid = left + (right - left) / 2;
    std::string restart_key;
    status = RestartKey(layout, mid, &restart_key);
    if (!status.ok()) {
      return status;
    }
    if (restart_key <= target) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }

  const std::uint32_t restart_index = left == 0 ? 0 : left - 1;
  const std::uint32_t start_offset =
      DecodeFixed32(layout.restart_base + restart_index * 4);
  const char* ptr = layout.payload.data() + start_offset;
  std::string previous_key;
  while (ptr < layout.records_end) {
    VersionedEntry entry;
    const char* next = nullptr;
    status = DecodePrefixRecordAt(layout, ptr, previous_key, &entry, &next);
    if (!status.ok()) {
      return status;
    }
    previous_key = entry.key;
    ptr = next;

    if (entry.key == key && entry.sequence <= read_sequence) {
      *found = true;
      if (!entry.deleted) {
        *value = entry.value;
      }
      return Status::OK();
    }
    if (entry.key > key) {
      return Status::OK();
    }
  }
  return Status::OK();
}

Status SSTable::DecodeBlock(const std::string& block_data,
                            std::vector<VersionedEntry>* entries) const {
  std::string_view payload;
  bool prefix_compressed = false;
  Status status = DecodeBlockPayload(block_data, &payload, &prefix_compressed);
  if (!status.ok()) {
    return status;
  }
  if (prefix_compressed) {
    return DecodePrefixCompressedPayload(payload, entries);
  }
  return DecodeLegacyBlockPayload(payload, entries);
}

}  // namespace kv
