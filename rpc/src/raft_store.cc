#include "kv_raft/store.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sys/file.h>
#include <unistd.h>
#include "storage_util.h"

namespace kv::raft {
namespace {
Status IO(const char* action) {
  return Status::IOError(std::string(action) + ": " + std::strerror(errno));
}
}
Store::~Store() { if (lock_fd_ >= 0) ::close(lock_fd_); }

Status Store::Open(const std::string& identity, wire::State* state) {
  if (lock_fd_ >= 0) return Status::IOError("store already open");
  // Parent must already exist so its durable directory chain is unambiguous.
  auto parent = root_.parent_path();
  if (parent.empty()) parent = ".";
  std::error_code ec;
  std::filesystem::create_directory(root_, ec);
  if (ec) return Status::IOError("create node directory: " + ec.message());
  auto status = FsyncDirectory(parent);
  if (!status.ok()) return status;
  lock_fd_ = ::open((root_ / "LOCK").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
    return Status::IOError("node directory is locked or inaccessible");
  const auto path = root_ / "raft.state";
  if (!std::filesystem::exists(path)) {
    for (const auto& entry : std::filesystem::directory_iterator(root_)) {
      if (entry.path().filename() != "LOCK")
        return Status::Corruption("state missing in nonempty node directory; refuse bootstrap");
    }
    state->Clear();
    state->set_version(1);
    state->set_identity(identity);
    return Save(*state);
  }
  const auto size = std::filesystem::file_size(path);
  if (size < 12 || size > kMaxStateBytes + 12)
    return Status::Corruption("invalid raft state length");
  std::ifstream input(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!input.read(bytes.data(), bytes.size())) return Status::IOError("read raft state");
  const std::string_view payload(bytes.data() + 12, bytes.size() - 12);
  if (bytes.compare(0, 4, "KRF1") != 0 || DecodeFixed32(bytes.data() + 4) != payload.size() ||
      DecodeFixed32(bytes.data() + 8) != CRC32(payload) ||
      !state->ParseFromArray(payload.data(), static_cast<int>(payload.size())) ||
      (state->version() != 1 && state->version() != 2) || state->identity() != identity ||
      state->voted_for() > 3 ||
      state->snapshot_index() > std::numeric_limits<std::uint64_t>::max() - state->entries_size() ||
      state->commit() < state->snapshot_index() ||
      state->commit() > state->snapshot_index() + state->entries_size() ||
      (state->snapshot_index() == 0) != (state->snapshot_term() == 0) ||
      (state->snapshot_index() == 0) != (state->snapshot_bytes() == 0) ||
      state->snapshot_term() > state->term() ||
      (state->version() == 1 && state->snapshot_index() != 0))
    return Status::Corruption("raft checksum, identity or state invalid");
  std::uint64_t previous = state->snapshot_term();
  for (const auto& entry : state->entries()) {
    if (entry.term() == 0 || entry.term() < previous || entry.term() > state->term())
      return Status::Corruption("invalid raft log term ordering");
    previous = entry.term();
  }
  return state->snapshot_index() == 0 ? Status::OK() : ValidateSnapshot(*state);
}

Status Store::Save(const wire::State& state) {
  std::string payload;
  if (state.ByteSizeLong() > kMaxStateBytes)
    return Status::IOError("retained Raft log exceeds 64 MiB");
  if (!state.SerializeToString(&payload)) return Status::IOError("serialize raft state");
  std::string bytes(12, '\0');
  bytes.replace(0, 4, "KRF1");
  EncodeFixed32(static_cast<std::uint32_t>(payload.size()), bytes.data() + 4);
  EncodeFixed32(CRC32(payload), bytes.data() + 8);
  bytes += payload;
  const auto temp = root_ / "raft.tmp";
  auto check = [&](const char* point) { return Check(point); };
  auto status = check("write");
  if (!status.ok()) return status;
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return IO("open raft.tmp");
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { status = IO("write raft.tmp"); break; }
    offset += static_cast<std::size_t>(count);
  }
  if (status.ok()) status = check("fsync");
  if (status.ok() && ::fsync(fd) != 0) status = IO("fsync raft.tmp");
  if (::close(fd) != 0 && status.ok()) status = IO("close raft.tmp");
  if (!status.ok()) return status;
  status = check("rename");
  if (!status.ok()) return status;
  if (::rename(temp.c_str(), (root_ / "raft.state").c_str()) != 0) return IO("rename raft state");
  status = check("directory");
  return status.ok() ? FsyncDirectory(root_) : status;
}
}  // namespace kv::raft
