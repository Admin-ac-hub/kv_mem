#include "sstable.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <set>
#include <utility>

#include "compression.h"
#include "format.h"

namespace kv {

namespace {

std::atomic<std::uint64_t> g_total_full_scan_count{0};

constexpr std::uint64_t kSSTableMagic = 0x4b565354424c4b33ULL;  // KVSTBLK3
constexpr size_t kFooterSize = 40;
constexpr size_t kRecordHeaderSize = 17;
constexpr std::uint32_t kDataBlockMagic = 0x344b4c42;  // BLK4
constexpr std::uint32_t kRestartInterval = 16;
constexpr size_t kBlockTrailerSize = 5;

std::string InternalKey(const std::string& user_key, SequenceNumber sequence) {
  const SequenceNumber inverted = std::numeric_limits<SequenceNumber>::max() - sequence;
  std::string out = user_key;
  out.push_back('\0');
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((inverted >> (i * 8)) & 0xff));
  }
  return out;
}

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

Status ReadExact(std::ifstream* stream, char* data, std::streamsize size) {
  stream->read(data, size);
  if (stream->gcount() != size) {
    return Status::Corruption("incomplete SSTable read");
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

  auto codec = NewCompressionCodec(CompressionType::kNone);
  std::string block;
  Status status = codec->Compress(payload, &block);
  if (!status.ok()) {
    return "";
  }
  block.push_back(static_cast<char>(codec->Type()));
  AppendFixed32(&block, CRC32(block));
  return block;
}

Status DecodeLegacyBlockPayload(std::string_view payload,
                                std::vector<VersionedEntry>* entries) {
  const char* ptr = payload.data();
  const char* end = ptr + payload.size();
  const std::uint64_t record_count = DecodeFixed64(ptr);
  ptr += 8;
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
    if (end - ptr < static_cast<std::ptrdiff_t>(key_size + value_size)) {
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
      case SSTable::ValueType::kMerge:
        return Status::Corruption("unsupported SSTable merge record");
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

Status DecodePrefixCompressedPayload(std::string_view payload,
                                     std::vector<VersionedEntry>* entries) {
  if (payload.size() < 20) {
    return Status::Corruption("invalid prefix-compressed SSTable block");
  }
  const char* ptr = payload.data();
  const char* end = ptr + payload.size();
  if (DecodeFixed32(ptr) != kDataBlockMagic) {
    return Status::Corruption("invalid prefix-compressed SSTable block magic");
  }
  ptr += 4;
  const std::uint32_t restart_interval = DecodeFixed32(ptr);
  ptr += 4;
  const std::uint64_t record_count = DecodeFixed64(ptr);
  ptr += 8;
  if (restart_interval == 0 || end - ptr < 4) {
    return Status::Corruption("invalid prefix-compressed SSTable restart metadata");
  }

  const std::uint32_t restart_count = DecodeFixed32(end - 4);
  const size_t restart_bytes = static_cast<size_t>(restart_count) * 4 + 4;
  if (payload.size() < 16 + restart_bytes) {
    return Status::Corruption("invalid prefix-compressed SSTable restart area");
  }
  const char* records_end = end - restart_bytes;

  entries->clear();
  entries->reserve(static_cast<size_t>(record_count));
  std::string previous_key;
  for (std::uint64_t i = 0; i < record_count; ++i) {
    if (records_end - ptr < 21) {
      return Status::Corruption("invalid prefix-compressed SSTable record");
    }
    const std::uint32_t shared = DecodeFixed32(ptr);
    const std::uint32_t unshared = DecodeFixed32(ptr + 4);
    const std::uint32_t value_size = DecodeFixed32(ptr + 8);
    const auto type = static_cast<SSTable::ValueType>(static_cast<std::uint8_t>(ptr[12]));
    const SequenceNumber sequence = DecodeFixed64(ptr + 13);
    ptr += 21;
    if (shared > previous_key.size() ||
        records_end - ptr < static_cast<std::ptrdiff_t>(unshared + value_size)) {
      return Status::Corruption("invalid prefix-compressed SSTable payload");
    }

    VersionedEntry entry;
    entry.key.assign(previous_key.data(), shared);
    entry.key.append(ptr, unshared);
    ptr += unshared;
    entry.sequence = sequence;
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
      case SSTable::ValueType::kMerge:
        return Status::Corruption("unsupported SSTable merge record");
      default:
        return Status::Corruption("unknown SSTable record type");
    }
    previous_key = entry.key;
    entries->push_back(std::move(entry));
  }
  if (ptr != records_end) {
    return Status::Corruption("trailing prefix-compressed SSTable record bytes");
  }
  return Status::OK();
}

Status DecodeCompressedBlockPayload(const std::string& block_data,
                                    std::string* payload,
                                    bool* prefix_compressed) {
  *prefix_compressed = false;
  if (block_data.size() >= kBlockTrailerSize + 16 &&
      DecodeFixed32(block_data.data()) == kDataBlockMagic) {
    const auto compression_type = static_cast<CompressionType>(
        static_cast<std::uint8_t>(block_data[block_data.size() - kBlockTrailerSize]));
    if (compression_type != CompressionType::kNone &&
        compression_type != CompressionType::kFake) {
      return Status::Corruption("unknown SSTable compression type");
    }
    const std::uint32_t expected_crc =
        DecodeFixed32(block_data.data() + block_data.size() - 4);
    const std::string_view checksummed(block_data.data(), block_data.size() - 4);
    if (CRC32(checksummed) != expected_crc) {
      return Status::Corruption("SSTable data block checksum mismatch");
    }
    auto codec = NewCompressionCodec(compression_type);
    if (codec == nullptr) {
      return Status::Corruption("unknown SSTable compression type");
    }
    Status status = codec->Uncompress(
        std::string_view(block_data.data(), block_data.size() - kBlockTrailerSize),
        payload);
    if (!status.ok()) {
      return status;
    }
    *prefix_compressed =
        payload->size() >= 4 && DecodeFixed32(payload->data()) == kDataBlockMagic;
    return Status::OK();
  }

  if (block_data.size() < 4) {
    return Status::Corruption("invalid SSTable data block");
  }
  const std::uint32_t expected_crc = DecodeFixed32(block_data.data() + block_data.size() - 4);
  const std::string_view legacy_payload(block_data.data(), block_data.size() - 4);
  if (CRC32(legacy_payload) != expected_crc) {
    return Status::Corruption("SSTable data block checksum mismatch");
  }
  payload->assign(legacy_payload.data(), legacy_payload.size());
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
  if (restart_interval == 0) {
    return Status::Corruption("invalid prefix-compressed SSTable restart interval");
  }
  const std::uint32_t restart_count = DecodeFixed32(end - 4);
  const size_t restart_bytes = static_cast<size_t>(restart_count) * 4 + 4;
  if (restart_count == 0 || payload.size() < 16 + restart_bytes) {
    return Status::Corruption("invalid prefix-compressed SSTable restart area");
  }

  layout->payload = payload;
  layout->records_begin = begin + 16;
  layout->records_end = end - restart_bytes;
  layout->restart_base = layout->records_end;
  layout->restart_count = restart_count;
  layout->record_count = DecodeFixed64(begin + 8);
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
  if (shared > previous_key.size() ||
      layout.records_end - ptr < static_cast<std::ptrdiff_t>(unshared + value_size)) {
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
    case SSTable::ValueType::kMerge:
      return Status::Corruption("unsupported SSTable merge record");
    default:
      return Status::Corruption("unknown SSTable record type");
  }
  *next = ptr;
  return Status::OK();
}

Status RestartKey(const PrefixBlockLayout& layout,
                  std::uint32_t restart_index,
                  std::string* key) {
  if (restart_index >= layout.restart_count) {
    return Status::Corruption("invalid prefix-compressed SSTable restart index");
  }
  const std::uint32_t offset = DecodeFixed32(layout.restart_base + restart_index * 4);
  const char* ptr = layout.payload.data() + offset;
  VersionedEntry entry;
  const char* next = nullptr;
  Status status = DecodePrefixRecordAt(layout, ptr, "", &entry, &next);
  if (!status.ok()) {
    return status;
  }
  *key = std::move(entry.key);
  return Status::OK();
}

}  // namespace

class SSTableInternalIterator : public InternalIterator {
 public:
  SSTableInternalIterator(const SSTable* table, SequenceNumber read_sequence)
      : table_(table), read_sequence_(read_sequence) {}

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

    const std::string internal_key = InternalKey(target, read_sequence_);
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

  const SSTable* table_;
  SequenceNumber read_sequence_ = 0;
  size_t block_index_ = 0;
  size_t entry_index_ = 0;
  bool loaded_block_ = false;
  std::vector<VersionedEntry> block_entries_;
  Status status_;
};

SSTable::SSTable(std::uint64_t file_number, std::filesystem::path path)
    : file_number_(file_number), path_(std::move(path)) {}

Status SSTable::CreateFromEntries(
    std::uint64_t file_number,
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
      index.push_back(IndexEntry{InternalKey(entries[end - 1].key, entries[end - 1].sequence),
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

  (void)file_number;
  return Status::OK();
}

Status SSTable::Open(std::uint64_t file_number,
                     const std::filesystem::path& path,
                     std::shared_ptr<BlockCache> block_cache,
                     std::shared_ptr<SSTable>* table) {
  auto candidate = std::shared_ptr<SSTable>(new SSTable(file_number, path));
  candidate->block_cache_ = std::move(block_cache);
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
    ++bloom_filtered_count_;
    return Status::NotFound("key not found");
  }

  const std::string target = InternalKey(key, read_sequence);
  auto it = std::lower_bound(index_.begin(), index_.end(), target,
                             [](const IndexEntry& entry, const std::string& target) {
                               return entry.last_internal_key < target;
                             });
  if (it == index_.end()) {
    return Status::NotFound("key not found");
  }

  std::vector<VersionedEntry> entries;
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
                    std::string* value) const {
  bool found = false;
  return Get(key, read_sequence, value, &found);
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
  ++full_scan_count_;
  ++g_total_full_scan_count;
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
  return std::make_unique<SSTableInternalIterator>(this, read_sequence);
}

std::uint64_t SSTable::FileNumber() const {
  return file_number_;
}

const std::filesystem::path& SSTable::FilePath() const {
  return path_;
}

std::uint64_t SSTable::BloomFilteredCount() const {
  return bloom_filtered_count_;
}

std::uint64_t SSTable::BlockReadCount() const {
  return block_read_count_;
}

std::uint64_t SSTable::FullScanCount() const {
  return full_scan_count_;
}

std::uint64_t SSTable::RestartSeekCount() const {
  return restart_seek_count_;
}

std::uint64_t SSTable::TotalFullScanCount() {
  return g_total_full_scan_count.load();
}

Status SSTable::LoadIndex() {
  std::ifstream stream(path_, std::ios::binary);
  if (!stream.is_open()) {
    return Status::IOError("failed to open SSTable: " + path_.string());
  }

  stream.seekg(0, std::ios::end);
  const auto file_size = static_cast<std::uint64_t>(stream.tellg());
  if (file_size < kFooterSize) {
    return Status::Corruption("SSTable too small: " + path_.string());
  }

  stream.seekg(static_cast<std::streamoff>(file_size - kFooterSize));
  std::array<char, kFooterSize> footer{};
  Status status = ReadExact(&stream, footer.data(), static_cast<std::streamsize>(footer.size()));
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
  if (index_offset + index_size != filter_offset ||
      filter_offset + filter_size + kFooterSize != file_size) {
    return Status::Corruption("invalid SSTable footer: " + path_.string());
  }

  std::string index_block(index_size, '\0');
  stream.seekg(static_cast<std::streamoff>(index_offset));
  status = ReadExact(&stream, index_block.data(), static_cast<std::streamsize>(index_size));
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
    index_.push_back(IndexEntry{std::move(last_internal_key), block_offset, block_size});
  }
  if (ptr != end) {
    return Status::Corruption("trailing SSTable index bytes");
  }

  std::string filter_block(filter_size, '\0');
  stream.seekg(static_cast<std::streamoff>(filter_offset));
  status = ReadExact(&stream, filter_block.data(), static_cast<std::streamsize>(filter_size));
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

  std::ifstream stream(path_, std::ios::binary);
  if (!stream.is_open()) {
    return Status::IOError("failed to open SSTable for read: " + path_.string());
  }
  block_data->assign(index.block_size, '\0');
  stream.seekg(static_cast<std::streamoff>(index.block_offset));
  Status status = ReadExact(&stream, block_data->data(), static_cast<std::streamsize>(index.block_size));
  if (!status.ok()) {
    return status;
  }
  ++block_read_count_;
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

  std::string payload;
  bool prefix_compressed = false;
  status = DecodeCompressedBlockPayload(block_data, &payload, &prefix_compressed);
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
  ++restart_seek_count_;

  std::uint32_t left = 0;
  std::uint32_t right = layout.restart_count;
  while (left + 1 < right) {
    const std::uint32_t mid = left + (right - left) / 2;
    std::string mid_key;
    status = RestartKey(layout, mid, &mid_key);
    if (!status.ok()) {
      return status;
    }
    if (mid_key <= key) {
      left = mid;
    } else {
      right = mid;
    }
  }

  const std::uint32_t start_offset = DecodeFixed32(layout.restart_base + left * 4);
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
  if (block_data.size() < 12) {
    return Status::Corruption("invalid SSTable data block");
  }

  if (block_data.size() >= kBlockTrailerSize + 16 &&
      DecodeFixed32(block_data.data()) == kDataBlockMagic) {
    const auto compression_type = static_cast<CompressionType>(
        static_cast<std::uint8_t>(block_data[block_data.size() - kBlockTrailerSize]));
    if (compression_type == CompressionType::kNone ||
        compression_type == CompressionType::kFake) {
      const std::uint32_t expected_crc =
          DecodeFixed32(block_data.data() + block_data.size() - 4);
      const std::string_view checksummed(block_data.data(), block_data.size() - 4);
      if (CRC32(checksummed) != expected_crc) {
        return Status::Corruption("SSTable data block checksum mismatch");
      }
      auto codec = NewCompressionCodec(compression_type);
      if (codec == nullptr) {
        return Status::Corruption("unknown SSTable compression type");
      }
      std::string payload;
      Status status = codec->Uncompress(
          std::string_view(block_data.data(), block_data.size() - kBlockTrailerSize),
          &payload);
      if (!status.ok()) {
        return status;
      }
      if (payload.size() >= 4 && DecodeFixed32(payload.data()) == kDataBlockMagic) {
        return DecodePrefixCompressedPayload(payload, entries);
      }
      return DecodeLegacyBlockPayload(payload, entries);
    }
  }

  const std::uint32_t expected_crc = DecodeFixed32(block_data.data() + block_data.size() - 4);
  const std::string_view payload(block_data.data(), block_data.size() - 4);
  if (CRC32(payload) != expected_crc) {
    return Status::Corruption("SSTable data block checksum mismatch");
  }
  return DecodeLegacyBlockPayload(payload, entries);
}

}  // namespace kv
