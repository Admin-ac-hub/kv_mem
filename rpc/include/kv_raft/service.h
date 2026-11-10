#pragma once
#include "kv_raft/node.h"
#include "minirpc/server/rpc_server.h"
namespace kv::raft {
void RegisterPeerService(minirpc::RpcServer& server, Node& node);
void RegisterClientService(minirpc::RpcServer& server, Node& node, const Config& config);
}  // namespace kv::raft
