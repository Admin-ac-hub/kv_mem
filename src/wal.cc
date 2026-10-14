#include "wal.h"

#include <array>
#include <limits>

#include "format.h"

namespace kv {

namespace {

constexpr std::uint32_t kWALBatchMagic = 0x41574c4b;  // KLWA, little-endian on disk.
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

}  // namespace

WALWriter::WALWriter(std::filesystem::path path, std::function<Status()> before_sync)
    : path_(std::move(path)), before_sync_(std::move(before_sync)) {}

WALWriter::~WALWriter() {
  Close();
}

Status WALWriter::Open() {
  Close();
  std::error_code ec;
  if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path(), ec);
  if (ec) return Status::IOError("failed to create WAL directory: " + ec.message());
  stream_.open(path_, std::ios::binary | std::ios::app);
  if (!stream_.is_open()) {
    return Status::IOError("failed to open WAL for append: " + path_.string());
  }
  // Persist the name of a newly created WAL before acknowledging its records.
  Status status = FsyncDirectory(path_.parent_path().empty() ? std::filesystem::path(".") : path_.parent_path());
  if (!status.ok()) Close();
  return status;
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
  Status status = batch.ValidateEncoding();
  if (!status.ok()) return status;
  if (sequence == 0 || batch.Count() - 1 > std::numeric_limits<SequenceNumber>::max() - sequence) {
    return Status::InvalidArgument("invalid WAL sequence range");
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
  if (before_sync_) {
    status = before_sync_();
    if (!status.ok()) return status;
  }
  return FsyncFile(path_);
}

WALReader::WALReader(std::filesystem::path path) : path_(std::move(path)) {}

Status WALReader::ReadNext(WALBatchRecord* record, bool* eof) {
  *eof = false;
  record->sequence = 0;
  record->batch.Clear();

  if (!opened_) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path_, ec);
    if (ec) return Status::IOError("failed to inspect WAL: " + ec.message());
    if (!exists) {
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
    return Status::Corruption("invalid WAL batch magic");
  }
  return ReadBatchRecord(record_offset, record, eof);
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

  if (count == 0 || sequence == 0 ||
      count - 1 > std::numeric_limits<SequenceNumber>::max() - sequence) {
    return Status::Corruption("invalid WAL sequence range or empty batch");
  }
  const auto payload_position = stream_.tellg();
  stream_.seekg(0, std::ios::end);
  const auto end_position = stream_.tellg();
  if (payload_position < 0 || end_position < payload_position) {
    return Status::IOError("failed to determine WAL size");
  }
  if (payload_size > static_cast<std::uint64_t>(end_position - payload_position)) {
    return TruncateTornTail(record_offset, eof);
  }
  stream_.seekg(payload_position);

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
