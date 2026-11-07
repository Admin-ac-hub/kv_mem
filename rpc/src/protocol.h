#pragma once

#include "kv_rpc/client.h"
#include "kv_service.pb.h"

namespace kv::rpc::detail {

inline Status ValidateKey(const std::string& key) {
  if (key.empty() || key.find_first_of(std::string("\t\n\0", 3)) != std::string::npos)
    return Status::InvalidArgument("key must be nonempty and contain no tab, newline or NUL");
  if (key.size() > kMaxPayloadBytes)
    return Status::InvalidArgument("key exceeds RPC payload limit");
  return Status::OK();
}

inline Status ValidateBatch(const WriteBatch& batch) {
  if (batch.Count() > kMaxBatchOperations)
    return Status::InvalidArgument("too many batch operations");
  std::size_t bytes = 0;
  for (const auto& op : batch.Operations()) {
    auto status = ValidateKey(op.key);
    if (!status.ok()) return status;
    if (op.type != WriteBatchOpType::kPut && op.type != WriteBatchOpType::kDelete)
      return Status::InvalidArgument("unknown operation type");
    if (op.value.size() > kMaxValueBytes)
      return Status::InvalidArgument("value exceeds 1 MiB RPC limit");
    if (op.type == WriteBatchOpType::kDelete && !op.value.empty())
      return Status::InvalidArgument("Delete cannot carry a value");
    bytes += op.key.size() + op.value.size();
    if (bytes > kMaxPayloadBytes)
      return Status::InvalidArgument("batch exceeds RPC payload limit");
  }
  return batch.ValidateEncoding();
}

inline wire::Reply::Code EncodeStatus(const Status& status) {
  if (status.ok()) return wire::Reply::OK;
  if (status.IsNotFound()) return wire::Reply::NOT_FOUND;
  if (status.IsInvalidArgument()) return wire::Reply::INVALID_ARGUMENT;
  if (status.IsCorruption()) return wire::Reply::CORRUPTION;
  return wire::Reply::IO_ERROR;
}

inline Status DecodeStatus(const wire::Reply& reply) {
  switch (reply.code()) {
    case wire::Reply::OK: return Status::OK();
    case wire::Reply::NOT_FOUND: return Status::NotFound(reply.message());
    case wire::Reply::INVALID_ARGUMENT: return Status::InvalidArgument(reply.message());
    case wire::Reply::IO_ERROR: return Status::IOError(reply.message());
    case wire::Reply::CORRUPTION: return Status::Corruption(reply.message());
    default: return Status::Corruption("unknown KV reply code");
  }
}

}  // namespace kv::rpc::detail
