#pragma once
#include <filesystem>
#include <functional>
#include "raft.pb.h"
#include "status.h"
#include "db.h"

namespace kv::raft {

// Bounds the retained log, independently of the streamed snapshot size.
inline constexpr std::size_t kMaxStateBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kSnapshotChunkBytes = 256 * 1024;
inline constexpr std::size_t kSnapshotLogBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kSnapshotLogEntries = 1024;
class Store {
 public:
  explicit Store(std::filesystem::path root) : root_(std::move(root)) {}
  ~Store();
  Status Open(const std::string& identity, wire::State* state);
  Status Save(const wire::State& state);
  Status CreateSnapshot(DB& db, std::uint64_t index, std::uint64_t term,
                        std::uint64_t* bytes);
  Status RestoreSnapshot(const wire::State& state, DB& db);
  Status ReadSnapshot(const wire::State& state, std::uint64_t offset, std::string* data);
  Status ReceiveSnapshot(const wire::InstallRequest& request, std::uint64_t* next);
  // Call only after the referencing raft.state has been durably published.
  Status RemoveObsoleteSnapshots(const wire::State& state);
  // Deterministic I/O fault boundaries; no production environment switches.
  std::function<Status(const char*)> before_io;
 private:
  std::filesystem::path SnapshotPath(std::uint64_t index, std::uint64_t term) const;
  Status ValidateSnapshot(const wire::State& state, DB* db = nullptr);
  Status PublishSnapshot(const std::filesystem::path& temp, std::uint64_t index,
                         std::uint64_t term);
  Status Check(const char* phase) const;
  std::filesystem::path root_;
  int lock_fd_ = -1;
  std::uint64_t receiving_index_ = 0, receiving_term_ = 0;
  std::uint64_t receiving_bytes_ = 0, received_ = 0;
};

}  // namespace kv::raft
