#include "kv_rpc/client.h"

#include <utility>

#include "protocol.h"

namespace kv::rpc {
namespace {

Result Invalid(Status status, bool mutation) {
  Result result;
  result.status = std::move(status);
  result.commit_state = mutation ? CommitState::NotCommitted : CommitState::NotApplicable;
  return result;
}

Result TransportFailure(minirpc::StatusCode code, const std::string& message,
                        bool mutation) {
  Result result;
  result.transport_status = minirpc::Status::Error(code, message);
  result.commit_state = mutation ? CommitState::Unknown : CommitState::NotApplicable;
  return result;
}

}  // namespace

std::string Result::ToString() const {
  return transport_status.ok() ? status.ToString() : transport_status.ToString();
}

Client::Client(minirpc::Endpoint endpoint) : client_(std::move(endpoint)) {}
void Client::Close() { client_.Close(); }

Result Client::Get(const std::string& key, std::chrono::milliseconds timeout) {
  auto status = detail::ValidateKey(key);
  if (!status.ok()) return Invalid(status, false);
  wire::GetRequest body;
  body.set_version(1);
  body.set_key(key);
  if (body.ByteSizeLong() > kMaxPayloadBytes)
    return Invalid(Status::InvalidArgument("Get payload exceeds RPC limit"), false);
  std::string payload;
  if (!body.SerializeToString(&payload))
    return TransportFailure(minirpc::StatusCode::kSerializeError, "cannot encode Get", false);
  return Call("Get", payload, false, timeout);
}

Result Client::Put(const std::string& key, const std::string& value,
                   std::chrono::milliseconds timeout) {
  WriteBatch batch;
  batch.Put(key, value);
  return Mutate("Put", batch, timeout);
}

Result Client::Delete(const std::string& key, std::chrono::milliseconds timeout) {
  WriteBatch batch;
  batch.Delete(key);
  return Mutate("Delete", batch, timeout);
}

Result Client::Write(const WriteBatch& batch, std::chrono::milliseconds timeout) {
  return Mutate("Write", batch, timeout);
}

Result Client::Mutate(const char* method, const WriteBatch& batch,
                      std::chrono::milliseconds timeout) {
  auto status = detail::ValidateBatch(batch);
  if (!status.ok()) return Invalid(status, true);
  wire::WriteRequest body;
  body.set_version(1);
  for (const auto& op : batch.Operations()) {
    auto* encoded = body.add_operations();
    encoded->set_type(op.type == WriteBatchOpType::kPut ? wire::Operation::PUT
                                                      : wire::Operation::DELETE);
    encoded->set_key(op.key);
    encoded->set_value(op.value);
  }
  if (body.ByteSizeLong() > kMaxPayloadBytes)
    return Invalid(Status::InvalidArgument("write payload exceeds RPC limit"), true);
  std::string payload;
  if (!body.SerializeToString(&payload)) {
    auto result = TransportFailure(minirpc::StatusCode::kSerializeError,
                                   "cannot encode write", true);
    result.commit_state = CommitState::NotCommitted;
    return result;
  }
  return Call(method, payload, true, timeout);
}

Result Client::Call(const char* method, const std::string& payload, bool mutation,
                    std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) {
    auto result = TransportFailure(minirpc::StatusCode::kTimeout, "timeout must be positive", mutation);
    if (mutation) result.commit_state = CommitState::NotCommitted;
    return result;
  }
  const auto response = client_.Call("KVService", method, payload, timeout);
  if (response.status_code != 0)
    return TransportFailure(static_cast<minirpc::StatusCode>(response.status_code),
                            response.error_message, mutation);
  wire::Reply reply;
  if (response.payload.size() > kMaxPayloadBytes ||
      !reply.ParseFromString(response.payload) || reply.version() != 1 ||
      reply.code() < wire::Reply::OK || reply.code() > wire::Reply::CORRUPTION ||
      reply.commit_state() < wire::Reply::NOT_APPLICABLE ||
      reply.commit_state() > wire::Reply::UNKNOWN ||
      (mutation && (reply.commit_state() == wire::Reply::NOT_APPLICABLE ||
                    ((reply.code() == wire::Reply::OK) !=
                     (reply.commit_state() == wire::Reply::COMMITTED)))) ||
      (!mutation && reply.commit_state() != wire::Reply::NOT_APPLICABLE) ||
      ((mutation || reply.code() != wire::Reply::OK) && !reply.value().empty()) ||
      reply.value().size() > kMaxValueBytes)
    return TransportFailure(minirpc::StatusCode::kProtocolError, "invalid KV reply", mutation);
  Result result;
  result.status = detail::DecodeStatus(reply);
  switch (reply.commit_state()) {
    case wire::Reply::NOT_APPLICABLE: result.commit_state = CommitState::NotApplicable; break;
    case wire::Reply::NOT_COMMITTED: result.commit_state = CommitState::NotCommitted; break;
    case wire::Reply::COMMITTED: result.commit_state = CommitState::Committed; break;
    case wire::Reply::UNKNOWN: result.commit_state = CommitState::Unknown; break;
    default: break;  // Rejected above.
  }
  result.value = std::move(*reply.mutable_value());
  return result;
}

}  // namespace kv::rpc
