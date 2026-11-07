#include <exception>
#include <iostream>
#include <string>

#include "cli.h"
#include "kv_rpc/client.h"

namespace {
int Usage() {
  std::cerr << "Usage: kv_client HOST PORT get KEY\n"
               "       kv_client HOST PORT put KEY VALUE\n"
               "       kv_client HOST PORT delete KEY\n"
               "       kv_client HOST PORT write (put KEY VALUE | delete KEY)...\n";
  return 2;
}
}

int main(int argc, char** argv) {
  if (argc < 5) return Usage();
  minirpc::Endpoint endpoint{argv[1], 0};
  if (!ParsePort(argv[2], &endpoint.port)) return Usage();
  const std::string method = argv[3];
  kv::WriteBatch batch;
  if (method == "write") {
    for (int i = 4; i < argc;) {
      const std::string operation = argv[i++];
      if (operation == "put" && i + 1 < argc) {
        batch.Put(argv[i], argv[i + 1]);
        i += 2;
      } else if (operation == "delete" && i < argc) {
        batch.Delete(argv[i++]);
      } else {
        return Usage();
      }
    }
  } else if (!((method == "get" || method == "delete") && argc == 5) &&
             !(method == "put" && argc == 6)) {
    return Usage();
  }
  try {
    kv::rpc::Client client(endpoint);
    kv::rpc::Result result;
    if (method == "get") result = client.Get(argv[4]);
    else if (method == "put") result = client.Put(argv[4], argv[5]);
    else if (method == "delete") result = client.Delete(argv[4]);
    else result = client.Write(batch);
    client.Close();
    if (!result.ok()) {
      std::cerr << result.ToString() << '\n';
      if (result.commit_state == kv::rpc::CommitState::Unknown) {
        std::cerr << "Write outcome unknown; do not blindly retry.\n";
        return 3;
      }
      return 1;
    }
    if (method == "get") std::cout << result.value << '\n';
    else std::cout << "OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (method != "get") {
      std::cerr << "Write outcome unknown; do not blindly retry.\n";
      return 3;
    }
    return 1;
  }
}
