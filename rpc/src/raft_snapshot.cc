#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#include "kv_raft/store.h"
#include "protocol.h"
#include "storage_util.h"

namespace kv::raft {
namespace {
constexpr std::size_t kHeaderBytes = 32;
struct File {
  int fd;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
  Status Close() {
    const int opened = fd;
    fd = -1;
    return ::close(opened) == 0 ? Status::OK() : Status::IOError("close snapshot file");
  }
};
Status WriteAll(int fd, std::string_view data) {
  while (!data.empty()) {
    const auto count = ::write(fd, data.data(), data.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return Status::IOError("write snapshot: " + std::string(std::strerror(errno)));
    data.remove_prefix(static_cast<std::size_t>(count));
  }
  return Status::OK();
}
}  // namespace
Status Store::Check(const char* phase) const { return before_io ? before_io(phase) : Status::OK(); }
std::filesystem::path Store::SnapshotPath(std::uint64_t index, std::uint64_t term) const {
  return root_ / ("raft-" + std::to_string(index) + "-" + std::to_string(term) + ".snapshot");
}
Status Store::PublishSnapshot(const std::filesystem::path& temp, std::uint64_t index,
                              std::uint64_t term) {
  auto status = Check("snapshot.fsync");
  if (status.ok()) status = FsyncFile(temp);
  if (status.ok()) status = Check("snapshot.rename");
  if (!status.ok()) return status;
  if (::rename(temp.c_str(), SnapshotPath(index, term).c_str()) != 0)
    return Status::IOError("rename snapshot: " + std::string(std::strerror(errno)));
  status = Check("snapshot.directory");
  return status.ok() ? FsyncDirectory(root_) : status;
}
Status Store::CreateSnapshot(DB& db, std::uint64_t index, std::uint64_t term,
                             std::uint64_t* bytes) {
  const auto temp = root_ / "snapshot.tmp";
  auto status = Check("snapshot.write");
  if (!status.ok()) return status;
  File file{::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
  if (file.fd < 0) return Status::IOError("open snapshot.tmp");
  std::string header(kHeaderBytes, '\0');
  header.replace(0, 4, "KSN1");
  EncodeFixed32(1, header.data() + 4);
  EncodeFixed64(index, header.data() + 8);
  EncodeFixed64(term, header.data() + 16);
  EncodeFixed32(CRC32(std::string_view(header).substr(0, 24)), header.data() + 24);
  status = WriteAll(file.fd, header);
  *bytes = header.size();
  auto iterator = db.NewIterator();
  iterator->SeekToFirst();
  // The caller holds the apply mutex for the entire scan: this view is exactly index.
  while (status.ok() && iterator->Valid()) {
    WriteBatch batch;
    std::size_t batch_bytes = 0;
    do {
      batch_bytes += 9 + iterator->key().size() + iterator->value().size();
      batch.Put(iterator->key(), iterator->value());
      iterator->Next();
    } while (iterator->Valid() && batch_bytes < kSnapshotChunkBytes &&
             batch_bytes + 9 + iterator->key().size() + iterator->value().size() <=
                 rpc::kMaxPayloadBytes &&
             batch.Count() < rpc::kMaxBatchOperations);
    const auto payload = batch.Encode();
    char record[8];
    EncodeFixed32(static_cast<std::uint32_t>(payload.size()), record);
    EncodeFixed32(CRC32(payload), record + 4);
    status = Check("snapshot.write");
    if (status.ok()) status = WriteAll(file.fd, std::string_view(record, 8));
    if (status.ok()) status = WriteAll(file.fd, payload);
    *bytes += 8 + payload.size();
  }
  if (status.ok()) status = iterator->status();
  if (status.ok()) status = Check("snapshot.finish");
  if (status.ok()) status = WriteAll(file.fd, std::string(8, '\0'));
  *bytes += 8;
  if (status.ok()) status = file.Close();
  if (status.ok()) status = PublishSnapshot(temp, index, term);
  return status;
}
Status Store::ValidateSnapshot(const wire::State& state, DB* db) {
  const auto path = SnapshotPath(state.snapshot_index(), state.snapshot_term());
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size != state.snapshot_bytes() || size < kHeaderBytes + 8)
    return Status::Corruption("snapshot missing or invalid length");
  std::ifstream input(path, std::ios::binary);
  char header[kHeaderBytes];
  if (!input.read(header, sizeof(header)) || std::memcmp(header, "KSN1", 4) != 0 ||
      DecodeFixed32(header + 4) != 1 || DecodeFixed64(header + 8) != state.snapshot_index() ||
      DecodeFixed64(header + 16) != state.snapshot_term() || DecodeFixed32(header + 28) != 0 ||
      DecodeFixed32(header + 24) != CRC32(std::string_view(header, 24)))
    return Status::Corruption("invalid snapshot header");
  std::uint64_t consumed = kHeaderBytes;
  std::string previous;
  while (consumed + 8 <= size) {
    char record[8];
    if (!input.read(record, sizeof(record))) return Status::Corruption("short snapshot record");
    consumed += 8;
    const auto length = DecodeFixed32(record);
    if (length == 0) {
      if (DecodeFixed32(record + 4) != 0 || consumed != size)
        return Status::Corruption("invalid snapshot end");
      return Status::OK();
    }
    if (length > rpc::kMaxPayloadBytes + 65536 || length > size - consumed)
      return Status::Corruption("invalid snapshot record length");
    std::string payload(length, '\0');
    if (!input.read(payload.data(), length) || DecodeFixed32(record + 4) != CRC32(payload))
      return Status::Corruption("snapshot record checksum mismatch");
    consumed += length;
    WriteBatch batch;
    auto status = batch.Decode(payload);
    if (!status.ok() || batch.Count() == 0 || batch.Count() > rpc::kMaxBatchOperations)
      return Status::Corruption("invalid snapshot batch");
    for (const auto& op : batch.Operations()) {
      if (op.type != WriteBatchOpType::kPut || !rpc::detail::ValidateKey(op.key).ok() ||
          op.value.size() > rpc::kMaxValueBytes || (!previous.empty() && op.key <= previous))
        return Status::Corruption("invalid snapshot key order or value");
      previous = op.key;
    }
    if (db) {
      status = Check("snapshot.restore.record");
      if (!status.ok()) return status;
      status = db->Write(batch);
      if (!status.ok()) return status;
    }
  }
  return Status::Corruption("snapshot end missing");
}
Status Store::RestoreSnapshot(const wire::State& state, DB& db) {
  if (state.snapshot_index() == 0) return Status::OK();
  return ValidateSnapshot(state, &db);
}
Status Store::ReadSnapshot(const wire::State& state, std::uint64_t offset, std::string* data) {
  if (offset >= state.snapshot_bytes()) return Status::InvalidArgument("snapshot offset past end");
  std::ifstream input(SnapshotPath(state.snapshot_index(), state.snapshot_term()),
                      std::ios::binary);
  input.seekg(static_cast<std::streamoff>(offset));
  data->resize(static_cast<std::size_t>(
      std::min<std::uint64_t>(kSnapshotChunkBytes, state.snapshot_bytes() - offset)));
  if (!input.read(data->data(), data->size())) return Status::IOError("read snapshot chunk");
  return Status::OK();
}
Status Store::ReceiveSnapshot(const wire::InstallRequest& request, std::uint64_t* next) {
  *next = 0;
  if (request.offset() == 0) {
    receiving_index_ = request.index();
    receiving_term_ = request.snapshot_term();
    receiving_bytes_ = request.total_bytes();
    received_ = 0;
  }
  if (receiving_index_ != request.index() || receiving_term_ != request.snapshot_term() ||
      receiving_bytes_ != request.total_bytes() || received_ != request.offset())
    return Status::InvalidArgument("snapshot transfer offset mismatch");
  const auto temp = root_ / "install.tmp";
  auto status = Check("install.write");
  if (!status.ok()) return status;
  File file{::open(temp.c_str(),
                   O_WRONLY | O_CREAT | O_CLOEXEC | (received_ == 0 ? O_TRUNC : O_APPEND), 0600)};
  if (file.fd < 0) return Status::IOError("open install.tmp");
  status = WriteAll(file.fd, request.data());
  if (!status.ok()) return status;
  status = file.Close();
  if (!status.ok()) return status;
  received_ += request.data().size();
  if (received_ == receiving_bytes_) {
    // Validate the complete stream before it can be referenced by raft.state.
    status = PublishSnapshot(temp, receiving_index_, receiving_term_);
    if (!status.ok()) return status;
    wire::State snapshot;
    snapshot.set_snapshot_index(receiving_index_);
    snapshot.set_snapshot_term(receiving_term_);
    snapshot.set_snapshot_bytes(receiving_bytes_);
    status = ValidateSnapshot(snapshot);
    if (!status.ok()) return status;
  }
  *next = received_;
  return Status::OK();
}
Status Store::RemoveObsoleteSnapshots(const wire::State& state) {
  auto status = Check("snapshot.cleanup");
  if (!status.ok()) return status;
  const auto active = SnapshotPath(state.snapshot_index(), state.snapshot_term());
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(root_)) {
    if (entry.path().extension() == ".snapshot" && entry.path() != active) {
      std::filesystem::remove(entry.path(), ec);
      if (ec) return Status::IOError("remove old snapshot: " + ec.message());
    }
  }
  status = Check("snapshot.cleanup.directory");
  return status.ok() ? FsyncDirectory(root_) : status;
}
}  // namespace kv::raft
