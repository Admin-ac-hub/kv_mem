#include "wal.h"

#include <array>
#include <limits>

#include "format.h"

namespace kv {

namespace {

constexpr std::uint32_t kWALBatchMagic = 0x41574c4b;  // KLWA, little-endian on disk.
constexpr size_t kLegacyHeaderSize = 17;
constexpr size_t kBatchHeaderWithoutMagicSize = 21;
constexpr size_t kBatchChecksumOffset = 17;

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

Status ReadPayload(std::ifstream* stream, std::string* payload, const std::string& what) {
  if (payload->empty()) {
    return Status::OK();
  }
  stream->read(payload->data(), static_cast<std::streamsize>(payload->size()));
  if (stream->gcount() != static_cast<std::streamsize>(payload->size())) {
    return Status::Corruption("incomplete WAL " + what);
  }
  return Status::OK();
}

}  // namespace

WALWriter::WALWriter(std::filesystem::path path) : path_(std::move(path)) {}

WALWriter::~WALWriter() {
  Close();
}

Status WALWriter::Open() {
  Close();
  std::filesystem::create_directories(path_.parent_path());
  stream_.open(path_, std::ios::binary | std::ios::app);
  if (!stream_.is_open()) {
    return Status::IOError("failed to open WAL for append: " + path_.string());
  }
  return Status::OK();
}

void WALWriter::Close() {
  if (stream_.is_open()) {
    stream_.close();
  }
}

Status WALWriter::AppendBatch(const WriteBatch& batch, SequenceNumber sequence) {
  if (batch.Count() == 0) {
    return Status::OK();
  }
  if (!stream_.is_open()) {
    return Status::IOError("WAL writer is not open");
  }
  if (batch.Count() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("WriteBatch operation count is too large");
  }

  const std::string payload = batch.Encode();
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("WriteBatch payload is too large");
  }

  std::string header;
  header.reserve(kBatchHeaderWithoutMagicSize);
  header.push_back(static_cast<char>(RecordType::kBatch));
  AppendFixed64(&header, sequence);
  AppendFixed32(&header, static_cast<std::uint32_t>(batch.Count()));
  AppendFixed32(&header, static_cast<std::uint32_t>(payload.size()));

  std::string checksummed = header;
  checksummed.append(payload);
  const std::uint32_t checksum = CRC32(checksummed);

  std::array<char, 4> magic{};
  EncodeFixed32(kWALBatchMagic, magic.data());
  std::array<char, 4> checksum_buf{};
  EncodeFixed32(checksum, checksum_buf.data());

  stream_.write(magic.data(), static_cast<std::streamsize>(magic.size()));
  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
  stream_.write(checksum_buf.data(), static_cast<std::streamsize>(checksum_buf.size()));
  stream_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  stream_.flush();

  if (!stream_) {
    return Status::IOError("failed to append WAL batch record");
  }
  return FsyncFile(path_);
}

WALReader::WALReader(std::filesystem::path path) : path_(std::move(path)) {}

Status WALReader::ReadNext(WALBatchRecord* record, bool* eof) {
  *eof = false;
  record->sequence = 0;
  record->batch.Clear();

  if (!opened_) {
    if (!std::filesystem::exists(path_)) {
      *eof = true;
      return Status::OK();
    }
    stream_.open(path_, std::ios::binary);
    if (!stream_.is_open()) {
      return Status::IOError("failed to open WAL for read: " + path_.string());
    }
    opened_ = true;
  }

  const std::streampos position = stream_.tellg();
  if (position == std::streampos(-1)) {
    return Status::IOError("failed to determine WAL record offset");
  }
  const auto record_offset = static_cast<std::uintmax_t>(position);
  std::array<char, 4> first_word{};
  stream_.read(first_word.data(), static_cast<std::streamsize>(first_word.size()));
  if (stream_.gcount() == 0) {
    *eof = true;
    return Status::OK();
  }
  if (stream_.gcount() != static_cast<std::streamsize>(first_word.size())) {
    if (!stream_.eof()) {
      return Status::IOError("failed to read WAL record prefix");
    }
    return TruncateTornTail(record_offset, eof);
  }

  const std::uint32_t word = DecodeFixed32(first_word.data());
  if (word != kWALBatchMagic) {
    return ReadLegacyRecord(word, record);
  }
  return ReadBatchRecord(record_offset, record, eof);
}

Status WALReader::ReadLegacyRecord(std::uint32_t expected_checksum,
                                   WALBatchRecord* record) {
  std::array<char, kLegacyHeaderSize> header{};
  stream_.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (stream_.gcount() != static_cast<std::streamsize>(header.size())) {
    return Status::Corruption("incomplete legacy WAL header");
  }

  const auto legacy_type = static_cast<WriteBatchOpType>(
      static_cast<std::uint8_t>(header[0]));
  if (legacy_type != WriteBatchOpType::kPut &&
      legacy_type != WriteBatchOpType::kDelete) {
    return Status::Corruption("unknown legacy WAL record type");
  }

  const SequenceNumber sequence = DecodeFixed64(header.data() + 1);
  const std::uint32_t key_size = DecodeFixed32(header.data() + 9);
  const std::uint32_t value_size = DecodeFixed32(header.data() + 13);

  std::string key(key_size, '\0');
  Status status = ReadPayload(&stream_, &key, "legacy key");
  if (!status.ok()) {
    return status;
  }
  std::string value(value_size, '\0');
  status = ReadPayload(&stream_, &value, "legacy value");
  if (!status.ok()) {
    return status;
  }

  std::string checksummed(header.data(), header.size());
  checksummed.append(key);
  checksummed.append(value);
  if (CRC32(checksummed) != expected_checksum) {
    return Status::Corruption("legacy WAL checksum mismatch");
  }

  record->sequence = sequence;
  if (legacy_type == WriteBatchOpType::kPut) {
    record->batch.Put(std::move(key), std::move(value));
  } else {
    record->batch.Delete(std::move(key));
  }
  return Status::OK();
}

Status WALReader::ReadBatchRecord(std::uintmax_t record_offset,
                                  WALBatchRecord* record,
                                  bool* eof) {
  std::array<char, kBatchHeaderWithoutMagicSize> header{};
  stream_.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (stream_.gcount() != static_cast<std::streamsize>(header.size())) {
    if (!stream_.eof()) {
      return Status::IOError("failed to read WAL batch header");
    }
    return TruncateTornTail(record_offset, eof);
  }

  const auto record_type = static_cast<RecordType>(static_cast<std::uint8_t>(header[0]));
  if (record_type != RecordType::kBatch) {
    return Status::Corruption("unknown WAL batch record type");
  }

  const SequenceNumber sequence = DecodeFixed64(header.data() + 1);
  const std::uint32_t count = DecodeFixed32(header.data() + 9);
  const std::uint32_t payload_size = DecodeFixed32(header.data() + 13);
  const std::uint32_t expected_checksum =
      DecodeFixed32(header.data() + kBatchChecksumOffset);

  std::string payload(payload_size, '\0');
  if (payload_size > 0) {
    stream_.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(payload.size())) {
      if (!stream_.eof()) {
        return Status::IOError("failed to read WAL batch payload");
      }
      return TruncateTornTail(record_offset, eof);
    }
  }

  std::string checksummed(header.data(), kBatchChecksumOffset);
  checksummed.append(payload);
  if (CRC32(checksummed) != expected_checksum) {
    return Status::Corruption("WAL batch checksum mismatch");
  }

  WriteBatch batch;
  Status status = batch.Decode(payload);
  if (!status.ok()) {
    return status;
  }
  if (batch.Count() != count) {
    return Status::Corruption("WAL batch operation count mismatch");
  }

  record->sequence = sequence;
  record->batch = std::move(batch);
  return Status::OK();
}

Status WALReader::TruncateTornTail(std::uintmax_t record_offset, bool* eof) {
  stream_.close();
  opened_ = false;

  std::error_code ec;
  std::filesystem::resize_file(path_, record_offset, ec);
  if (ec) {
    return Status::IOError("failed to truncate torn WAL: " + ec.message());
  }
  Status status = FsyncFile(path_);
  if (!status.ok()) {
    return status;
  }
  status = FsyncDirectory(path_.parent_path());
  if (!status.ok()) {
    return status;
  }
  *eof = true;
  return Status::OK();
}

}  // namespace kv
