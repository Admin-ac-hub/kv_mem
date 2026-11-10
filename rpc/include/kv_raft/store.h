#pragma once
#include <filesystem>
#include <functional>
#include "raft.pb.h"
#include "status.h"

namespace kv::raft {

// Full history is deliberately retained. No log compaction in this version.
inline constexpr std::size_t kMaxStateBytes = 64 * 1024 * 1024;
class Store {
 public:
  explicit Store(std::filesystem::path root) : root_(std::move(root)) {}
  ~Store();
  Status Open(const std::string& identity, wire::State* state);
  Status Save(const wire::State& state);
  // Deterministic I/O fault boundaries; no production environment switches.
  std::function<Status(const char*)> before_io;
 private:
  std::filesystem::path root_;
  int lock_fd_ = -1;
};

}  // namespace kv::raft
