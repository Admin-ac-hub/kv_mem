#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include "minirpc/core/endpoint.h"
#include "minirpc/protocol/message.h"
#include "status.h"

namespace kv::raft {
struct Config {
  std::string cluster;
  std::array<minirpc::Endpoint, 3> clients;
  std::array<minirpc::Endpoint, 3> peers;
  Status Validate() const;
  std::string Identity(unsigned id) const;
  static Status Load(const std::filesystem::path& path, Config* config);
};
// A single bounded, non-replaying miniRPC exchange, including connect/send.
minirpc::RpcResponse Exchange(const minirpc::Endpoint& endpoint,
                             const char* service, const char* method,
                             const std::string& payload,
                             std::chrono::milliseconds timeout);
}  // namespace kv::raft
