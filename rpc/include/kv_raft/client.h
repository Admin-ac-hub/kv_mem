#pragma once
#include "kv_raft/transport.h"
#include "kv_rpc/client.h"
#include "raft.pb.h"
namespace kv::raft {
// Not thread-safe. No replay after a mutation was sent without a known rejection.
class Client {
 public:
  explicit Client(Config config) : config_(std::move(config)) {}
  wire::Info Info(unsigned id);
  rpc::Result Get(const std::string& key);
  rpc::Result Write(const WriteBatch& batch);
 private:
  rpc::Result Call(const char* method, const std::string& payload, bool mutation);
  Config config_;
  unsigned leader_ = 0;
};
}  // namespace kv::raft
