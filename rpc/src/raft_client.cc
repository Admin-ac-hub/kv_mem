#include "kv_raft/client.h"
#include <thread>
#include "protocol.h"
namespace kv::raft {
using namespace std::chrono_literals;
wire::Info Client::Info(unsigned id) {
  wire::Info info;
  info.set_error("node unavailable or invalid response");
  if (id < 1 || id > 3 || !config_.Validate().ok()) return info;
  wire::ClientRequest request;
  request.set_cluster(config_.Identity(0));
  const auto response = Exchange(config_.clients[id - 1], "ClusterKV", "Info", request.SerializeAsString(), 200ms);
  wire::Info decoded;
  if (response.status_code == 0 && decoded.ParseFromString(response.payload) &&
      decoded.id() == id && decoded.leader() <= 3) return decoded;
  return info;
}
rpc::Result Client::Get(const std::string& key) {
  auto status = rpc::detail::ValidateKey(key);
  if (!status.ok()) { rpc::Result result; result.status = status; return result; }
  rpc::wire::GetRequest request;
  request.set_version(1);
  request.set_key(key);
  return Call("Get", request.SerializeAsString(), false);
}
rpc::Result Client::Write(const WriteBatch& batch) {
  auto status = rpc::detail::ValidateBatch(batch);
  if (!status.ok()) {
    rpc::Result result;
    result.status = status;
    result.commit_state = rpc::CommitState::NotCommitted;
    return result;
  }
  rpc::wire::WriteRequest request;
  request.set_version(1);
  for (const auto& op : batch.Operations()) {
    auto* encoded = request.add_operations();
    encoded->set_type(op.type == WriteBatchOpType::kPut ? rpc::wire::Operation::PUT : rpc::wire::Operation::DELETE);
    encoded->set_key(op.key);
    encoded->set_value(op.value);
  }
  return Call("Write", request.SerializeAsString(), true);
}
rpc::Result Client::Call(const char* method, const std::string& payload, bool mutation) {
  rpc::Result result;
  result.commit_state = mutation ? rpc::CommitState::NotCommitted : rpc::CommitState::NotApplicable;
  result.status = config_.Validate();
  if (!result.status.ok()) return result;
  if (payload.size() > rpc::kMaxPayloadBytes) {
    result.status = Status::InvalidArgument("cluster payload exceeds limit");
    return result;
  }
  wire::ClientRequest request;
  request.set_cluster(config_.Identity(0));
  request.set_payload(payload);
  // Discovery is side-effect free. Retry only discovery or explicit pre-admission rejection.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (leader_ == 0) {
      std::uint64_t newest_term = 0;
      for (unsigned id = 1; id <= 3; ++id) {
        const auto info = Info(id);
        if (info.error().empty() && info.is_leader() && info.term() >= newest_term) {
          leader_ = id;
          newest_term = info.term();
        }
      }
      if (leader_ == 0) { std::this_thread::sleep_for(100ms); continue; }
    }
    if (mutation) result.commit_state = rpc::CommitState::Unknown;
    const auto response = Exchange(config_.clients[leader_ - 1], "ClusterKV", method,
                                   request.SerializeAsString(), 2200ms);
    leader_ = 0;
    if (response.status_code != 0) {
      if (!mutation) continue;
      result.transport_status = minirpc::Status::Error(
          static_cast<minirpc::StatusCode>(response.status_code), response.error_message);
      return result;
    }
    wire::ClientReply reply;
    if (!reply.ParseFromString(response.payload) || !reply.has_result() || reply.leader() > 3) {
      result.status = Status::Corruption("invalid cluster reply");
      return result;
    }
    const auto& value = reply.result();
    if (value.version() != 1 || value.code() < rpc::wire::Reply::OK || value.code() > rpc::wire::Reply::CORRUPTION ||
        value.commit_state() < rpc::wire::Reply::NOT_APPLICABLE || value.commit_state() > rpc::wire::Reply::UNKNOWN ||
        (mutation && (value.commit_state() == rpc::wire::Reply::NOT_APPLICABLE ||
          ((value.code() == rpc::wire::Reply::OK) != (value.commit_state() == rpc::wire::Reply::COMMITTED)))) ||
        (!mutation && value.commit_state() != rpc::wire::Reply::NOT_APPLICABLE) ||
        ((mutation || value.code() != rpc::wire::Reply::OK) && !value.value().empty()) ||
        value.value().size() > rpc::kMaxValueBytes ||
        (reply.rejected() && (value.code() == rpc::wire::Reply::OK ||
          (mutation && value.commit_state() != rpc::wire::Reply::NOT_COMMITTED)))) {
      result.status = Status::Corruption("invalid cluster response semantics");
      return result;
    }
    if (reply.rejected()) {
      result.commit_state = mutation ? rpc::CommitState::NotCommitted : rpc::CommitState::NotApplicable;
      std::this_thread::sleep_for(100ms);
      continue;
    }
    result.status = rpc::detail::DecodeStatus(value);
    result.value = value.value();
    switch (value.commit_state()) {
      case rpc::wire::Reply::NOT_APPLICABLE: result.commit_state = rpc::CommitState::NotApplicable; break;
      case rpc::wire::Reply::NOT_COMMITTED: result.commit_state = rpc::CommitState::NotCommitted; break;
      case rpc::wire::Reply::COMMITTED: result.commit_state = rpc::CommitState::Committed; break;
      case rpc::wire::Reply::UNKNOWN: result.commit_state = rpc::CommitState::Unknown; break;
      default: break;
    }
    return result;
  }
  result.status = Status::IOError("no available majority leader");
  return result;
}
}  // namespace kv::raft
