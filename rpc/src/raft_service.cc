#include "kv_raft/service.h"
#include "protocol.h"

namespace kv::raft {
namespace {
template <class Message>
minirpc::RpcResponse Response(const minirpc::RpcRequest& request, const Message& message) {
  minirpc::RpcResponse response;
  response.request_id = request.request_id;
  if (!message.SerializeToString(&response.payload)) {
    response.status_code = static_cast<int>(minirpc::StatusCode::kSerializeError);
    response.error_message = "cannot serialize cluster reply";
  }
  return response;
}
minirpc::RpcResponse Invalid(const minirpc::RpcRequest& request) {
  minirpc::RpcResponse response;
  response.request_id = request.request_id;
  response.status_code = static_cast<int>(minirpc::StatusCode::kProtocolError);
  response.error_message = "invalid cluster request";
  return response;
}
Status Decode(const char* method, const std::string& payload, WriteBatch* batch) {
  rpc::wire::WriteRequest body;
  if (!body.ParseFromString(payload) || body.version() != 1 ||
      body.operations_size() > static_cast<int>(rpc::kMaxBatchOperations))
    return Status::InvalidArgument("invalid write payload");
  const std::string name(method);
  if (name != "Write" && body.operations_size() != 1)
    return Status::InvalidArgument("Put/Delete require one operation");
  for (const auto& op : body.operations()) {
    if ((name == "Put" && op.type() != rpc::wire::Operation::PUT) ||
        (name == "Delete" && op.type() != rpc::wire::Operation::DELETE))
      return Status::InvalidArgument("operation/method mismatch");
    if (op.type() == rpc::wire::Operation::PUT) batch->Put(op.key(), op.value());
    else if (op.type() == rpc::wire::Operation::DELETE && op.value().empty()) batch->Delete(op.key());
    else return Status::InvalidArgument("invalid batch operation");
  }
  return rpc::detail::ValidateBatch(*batch);
}
}
void RegisterPeerService(minirpc::RpcServer& server, Node& node) {
  server.RegisterService("Raft", "Vote", [&node](const auto& request) {
    wire::VoteRequest body;
    if (request.payload.size() > 4096 || !body.ParseFromString(request.payload)) return Invalid(request);
    return Response(request, node.Vote(body));
  });
  server.RegisterService("Raft", "Append", [&node](const auto& request) {
    wire::AppendRequest body;
    if (request.payload.size() > rpc::kMaxPayloadBytes + 131072 || !body.ParseFromString(request.payload))
      return Invalid(request);
    return Response(request, node.Append(body));
  });
}
void RegisterClientService(minirpc::RpcServer& server, Node& node, const Config& config) {
  for (const char* method : {"Info", "Get", "Put", "Delete", "Write"}) {
    server.RegisterService("ClusterKV", method, [&node, identity = config.Identity(0), method](const auto& request) {
      wire::ClientRequest envelope;
      if (request.payload.size() > rpc::kMaxPayloadBytes + 4096 ||
          !envelope.ParseFromString(request.payload) || envelope.cluster() != identity ||
          envelope.payload().size() > rpc::kMaxPayloadBytes) return Invalid(request);
      if (std::string(method) == "Info") return Response(request, node.Info());
      const bool mutation = std::string(method) != "Get";
      rpc::Result result;
      result.commit_state = mutation ? rpc::CommitState::NotCommitted : rpc::CommitState::NotApplicable;
      if (mutation) {
        WriteBatch batch;
        result.status = Decode(method, envelope.payload(), &batch);
        if (result.status.ok()) result = node.Execute(&batch);
      } else {
        rpc::wire::GetRequest body;
        if (!body.ParseFromString(envelope.payload()) || body.version() != 1)
          result.status = Status::InvalidArgument("invalid Get payload");
        else result = node.Execute(nullptr, body.key());
      }
      wire::ClientReply reply;
      const auto info = node.Info();
      reply.set_leader(info.leader());
      // Only a request known not to have entered the log can be redirected.
      reply.set_rejected(result.status.IsIOError() &&
          (mutation ? result.commit_state == rpc::CommitState::NotCommitted : true));
      auto* encoded = reply.mutable_result();
      encoded->set_version(1);
      encoded->set_code(rpc::detail::EncodeStatus(result.status));
      if (!result.status.ok()) encoded->set_message(result.status.ToString());
      encoded->set_value(std::move(result.value));
      switch (result.commit_state) {
        case rpc::CommitState::NotApplicable: encoded->set_commit_state(rpc::wire::Reply::NOT_APPLICABLE); break;
        case rpc::CommitState::NotCommitted: encoded->set_commit_state(rpc::wire::Reply::NOT_COMMITTED); break;
        case rpc::CommitState::Committed: encoded->set_commit_state(rpc::wire::Reply::COMMITTED); break;
        case rpc::CommitState::Unknown: encoded->set_commit_state(rpc::wire::Reply::UNKNOWN); break;
      }
      return Response(request, reply);
    });
  }
}
}  // namespace kv::raft
