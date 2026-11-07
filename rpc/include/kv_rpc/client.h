#pragma once

#include <chrono>
#include <string>

#include "minirpc/client/rpc_client.h"
#include "status.h"
#include "write_batch.h"

namespace kv::rpc {

inline constexpr std::size_t kMaxPayloadBytes = 1536 * 1024;
inline constexpr std::size_t kMaxValueBytes = 1024 * 1024;
inline constexpr std::size_t kMaxBatchOperations = 4096;

enum class CommitState { NotApplicable, NotCommitted, Committed, Unknown };

struct Result {
  minirpc::Status transport_status;
  Status status = Status::IOError("no KV response");
  CommitState commit_state = CommitState::NotApplicable;
  std::string value;

  bool ok() const { return transport_status.ok() && status.ok(); }
  std::string ToString() const;
};

// No automatic replay of mutations: a timeout may follow a durable write.
class Client {
 public:
  explicit Client(minirpc::Endpoint endpoint);
  void Close();
  Result Get(const std::string& key,
             std::chrono::milliseconds timeout = std::chrono::seconds(3));
  Result Put(const std::string& key, const std::string& value,
             std::chrono::milliseconds timeout = std::chrono::seconds(3));
  Result Delete(const std::string& key,
                std::chrono::milliseconds timeout = std::chrono::seconds(3));
  Result Write(const WriteBatch& batch,
               std::chrono::milliseconds timeout = std::chrono::seconds(3));

 private:
  Result Mutate(const char* method, const WriteBatch& batch,
                std::chrono::milliseconds timeout);
  Result Call(const char* method, const std::string& payload, bool mutation,
              std::chrono::milliseconds timeout);
  minirpc::RpcClient client_;
};

}  // namespace kv::rpc
