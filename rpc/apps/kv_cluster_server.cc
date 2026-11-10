#include <charconv>
#include <csignal>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include "kv_raft/service.h"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: kv_cluster_server CONFIG NODE_ID DATA_DIR\n";
    return 2;
  }
  unsigned id = 0;
  const auto parsed = std::from_chars(argv[2], argv[2] + std::strlen(argv[2]), id);
  kv::raft::Config config;
  auto status = kv::raft::Config::Load(argv[1], &config);
  if (!status.ok() || parsed.ec != std::errc{} || *parsed.ptr != '\0' || id < 1 || id > 3) {
    std::cerr << (status.ok() ? "NODE_ID must be 1..3" : status.ToString()) << '\n';
    return 2;
  }
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) return 1;
  try {
    kv::raft::Node node(config, id, argv[3]);
    status = node.Open();
    if (!status.ok()) { std::cerr << status.ToString() << '\n'; return 1; }
    minirpc::RpcServer peers, clients;
    kv::raft::RegisterPeerService(peers, node);
    kv::raft::RegisterClientService(clients, node, config);
    auto started = peers.Start(config.peers[id - 1]);
    if (started.ok()) started = clients.Start(config.clients[id - 1]);
    if (!started.ok()) { std::cerr << started.ToString() << '\n'; return 1; }
    node.Start();
    std::cout << "node " << id << " client=" << config.clients[id - 1].ToString()
              << " peer=" << config.peers[id - 1].ToString() << std::endl;
    int received = 0;
    const int error = sigwait(&signals, &received);
    node.Stop();
    clients.Stop(std::chrono::seconds(2));
    peers.Stop(std::chrono::seconds(2));
    return error == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
