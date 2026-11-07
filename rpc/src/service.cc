#include "kv_rpc/service.h"

#include <string>
#include <utility>

#include "protocol.h"

namespace kv::rpc {
namespace {

minirpc::RpcResponse Respond(const minirpc::RpcRequest& request,
                             const Status& status,
                             wire::Reply::CommitState state,
                             std::string value = {}) {
  wire::Reply reply;
  reply.set_version(1);
  reply.set_code(detail::EncodeStatus(status));
  if (!status.ok()) reply.set_message(status.ToString());
  reply.set_commit_state(state);
  reply.set_value(std::move(value));
  minirpc::RpcResponse response;
  response.request_id = request.request_id;
  if (!reply.SerializeToString(&response.payload)) {
    response.status_code = static_cast<int>(minirpc::StatusCode::kSerializeError);
    response.error_message = "cannot encode KV reply";
  }
  return response;
}

minirpc::RpcResponse Get(DB& db, const minirpc::RpcRequest& request) {
  wire::GetRequest body;
  if (request.payload.size() > kMaxPayloadBytes ||
      !body.ParseFromString(request.payload) || body.version() != 1)
    return Respond(request, Status::InvalidArgument("invalid Get payload or version"),
                   wire::Reply::NOT_APPLICABLE);
  auto status = detail::ValidateKey(body.key());
  std::string value;
  if (status.ok()) status = db.Get(body.key(), &value);
  // Local databases may contain values larger than this service's contract.
  if (status.ok() && value.size() > kMaxValueBytes)
    status = Status::InvalidArgument("stored value exceeds 1 MiB RPC limit");
  if (!status.ok()) value.clear();
  return Respond(request, status, wire::Reply::NOT_APPLICABLE, std::move(value));
}

Status DecodeWrite(const minirpc::RpcRequest& request, WriteBatch* batch) {
  wire::WriteRequest body;
  if (request.payload.size() > kMaxPayloadBytes ||
      !body.ParseFromString(request.payload) || body.version() != 1)
    return Status::InvalidArgument("invalid write payload or version");
  if (body.operations_size() > static_cast<int>(kMaxBatchOperations))
    return Status::InvalidArgument("too many batch operations");
  if (request.method_name != "Write" && body.operations_size() != 1)
    return Status::InvalidArgument("Put/Delete require exactly one operation");
  for (const auto& op : body.operations()) {
    if ((request.method_name == "Put" && op.type() != wire::Operation::PUT) ||
        (request.method_name == "Delete" && op.type() != wire::Operation::DELETE))
      return Status::InvalidArgument("operation does not match RPC method");
    switch (op.type()) {
      case wire::Operation::PUT:
        batch->Put(op.key(), op.value());
        break;
      case wire::Operation::DELETE:
        if (!op.value().empty())
          return Status::InvalidArgument("Delete cannot carry a value");
        batch->Delete(op.key());
        break;
      default:
        return Status::InvalidArgument("unknown write operation");
    }
  }
  return detail::ValidateBatch(*batch);
}

minirpc::RpcResponse Write(DB& db, const minirpc::RpcRequest& request) {
  WriteBatch batch;
  auto status = DecodeWrite(request, &batch);
  if (!status.ok()) return Respond(request, status, wire::Reply::NOT_COMMITTED);
  status = db.Write(batch);
  // An error can follow WAL sync or MemTable publication. Do not promise rollback.
  return Respond(request, status, status.ok() ? wire::Reply::COMMITTED : wire::Reply::UNKNOWN);
}

}  // namespace

void RegisterService(minirpc::RpcServer& server, DB& db) {
  server.RegisterService("KVService", "Get",
                         [&db](const auto& request) { return Get(db, request); });
  for (const char* method : {"Put", "Delete", "Write"})
    server.RegisterService("KVService", method,
                           [&db](const auto& request) { return Write(db, request); });
}

}  // namespace kv::rpc
