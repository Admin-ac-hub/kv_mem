#include <iostream>
#include "kv_raft/client.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: kv_cluster_client CONFIG status|get KEY|put KEY VALUE|delete KEY|write (put KEY VALUE|delete KEY)...\n";
    return 2;
  }
  try {
    kv::raft::Config config;
    auto status = kv::raft::Config::Load(argv[1], &config);
    if (!status.ok()) { std::cerr << status.ToString() << '\n'; return 2; }
    kv::raft::Client client(config);
    const std::string method = argv[2];
    if (method == "status" && argc == 3) {
      for (unsigned id = 1; id <= 3; ++id) {
        const auto info = client.Info(id);
        std::cout << "node=" << id << " leader=" << info.leader() << " term=" << info.term()
                  << " committed=" << info.commit() << " last=" << info.last_index()
                  << " applied=" << info.applied_index() << " snapshot=" << info.snapshot_index()
                  << " role=" << (info.is_leader() ? "leader" : "follower")
                  << " error=" << info.error() << '\n';
      }
      return 0;
    }
    kv::WriteBatch batch;
    if (method == "put" && argc == 5) batch.Put(argv[3], argv[4]);
    else if (method == "delete" && argc == 4) batch.Delete(argv[3]);
    else if (method == "write") {
      for (int i = 3; i < argc;) {
        const std::string operation = argv[i++];
        if (operation == "put" && i + 1 < argc) { batch.Put(argv[i], argv[i + 1]); i += 2; }
        else if (operation == "delete" && i < argc) batch.Delete(argv[i++]);
        else { std::cerr << "invalid batch arguments\n"; return 2; }
      }
    } else if (!(method == "get" && argc == 4)) {
      std::cerr << "invalid command or arguments\n";
      return 2;
    }
    const auto result = method == "get" ? client.Get(argv[3]) : client.Write(batch);
    if (!result.ok()) {
      std::cerr << result.ToString() << '\n';
      if (result.commit_state == kv::rpc::CommitState::Unknown) {
        std::cerr << "Write outcome unknown; do not blindly retry.\n";
        return 3;
      }
      return 1;
    }
    std::cout << (method == "get" ? result.value : "OK") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nWrite outcome may be unknown; do not blindly retry.\n";
    return 3;
  }
}
