#pragma once

#include "db.h"
#include "minirpc/server/rpc_server.h"

namespace kv::rpc {

// DB must be open before Start; Stop must join handlers before DB is closed.
// The referenced DB must outlive server. Register before starting the server.
void RegisterService(minirpc::RpcServer& server, DB& db);

}  // namespace kv::rpc
