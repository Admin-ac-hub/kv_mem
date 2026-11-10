#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "kv_raft/client.h"

extern char** environ;
using namespace std::chrono_literals;
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <class Predicate>
void Wait(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 12s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::sleep_for(40ms);
  }
  throw std::runtime_error(message);
}
struct Cluster {
  kv::raft::Config config;
  std::filesystem::path root, config_path, server;
  std::array<pid_t, 3> pids{};
  explicit Cluster(std::filesystem::path executable) : server(std::move(executable)) {
    char directory[] = "/tmp/kv-cluster-test.XXXXXX";
    Require(::mkdtemp(directory) != nullptr, "mkdtemp failed");
    root = directory;
    config_path = root / "cluster.conf";
    config.cluster = "process-test";
    std::array<int, 6> sockets{};
    for (unsigned i = 0; i < 6; ++i) {
      sockets[i] = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      Require(sockets[i] >= 0, "socket failed");
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      Require(::bind(sockets[i], reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed");
      socklen_t length = sizeof(address);
      Require(::getsockname(sockets[i], reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname failed");
      auto& endpoint = i < 3 ? config.clients[i] : config.peers[i - 3];
      endpoint = {"127.0.0.1", ntohs(address.sin_port)};
    }
    for (int fd : sockets) ::close(fd);
    std::ofstream output(config_path);
    output << config.cluster << '\n';
    for (unsigned i = 0; i < 3; ++i)
      output << i + 1 << ' ' << config.clients[i].ToString() << ' ' << config.peers[i].ToString() << '\n';
  }
  ~Cluster() { for (unsigned id = 1; id <= 3; ++id) Kill(id); }
  void Start(unsigned id) {
    Require(pids[id - 1] == 0, "node already started");
    const auto name = std::to_string(id);
    const auto directory = (root / ("node" + name)).string();
    const auto log = (root / ("node" + name + ".log")).string();
    posix_spawn_file_actions_t actions;
    Require(posix_spawn_file_actions_init(&actions) == 0, "spawn init failed");
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    char* arguments[] = {const_cast<char*>(server.c_str()), const_cast<char*>(config_path.c_str()),
        const_cast<char*>(name.c_str()), const_cast<char*>(directory.c_str()), nullptr};
    const int error = posix_spawn(&pids[id - 1], server.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    Require(error == 0, "spawn server failed");
  }
  void Kill(unsigned id) {
    auto& pid = pids[id - 1];
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    pid = 0;
  }
  unsigned Leader(kv::raft::Client& client, unsigned excluded = 0) {
    unsigned leader = 0;
    std::uint64_t term = 0;
    for (unsigned id = 1; id <= 3; ++id) {
      if (id == excluded || pids[id - 1] == 0) continue;
      const auto info = client.Info(id);
      if (info.error().empty() && info.is_leader() && info.term() >= term) {
        leader = id;
        term = info.term();
      }
    }
    return leader;
  }
};
void Value(kv::raft::Client& client, const std::string& key, const std::string& expected) {
  const auto result = client.Get(key);
  Require(result.ok() && result.value == expected, ("wrong value: " + key + ": " + result.ToString()).c_str());
}
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  Cluster cluster(argv[1]);
  std::cout << "Process logs: " << cluster.root << std::endl;
  try {
    for (unsigned id = 1; id <= 3; ++id) cluster.Start(id);
    kv::raft::Client client(cluster.config);
    unsigned first = 0;
    Wait([&] { first = cluster.Leader(client); return first != 0; }, "initial election timed out");
    kv::WriteBatch batch;
    batch.Put("acknowledged", "before-crash");
    batch.Put("deleted", "temporary");
    batch.Delete("deleted");
    batch.Put("binary", std::string("a\0b", 3));
    batch.Put("empty", "");
    Require(client.Write(batch).ok(), "initial committed batch failed");
    auto cli = [&](std::vector<std::string> arguments, const std::string& expected) {
      arguments.insert(arguments.begin(), {argv[2], cluster.config_path.string()});
      std::vector<char*> pointers;
      for (auto& argument : arguments) pointers.push_back(argument.data());
      pointers.push_back(nullptr);
      const auto output_path = cluster.root / "cli.log";
      posix_spawn_file_actions_t actions;
      Require(posix_spawn_file_actions_init(&actions) == 0, "CLI spawn init failed");
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
      pid_t pid = 0;
      const auto error = posix_spawn(&pid, argv[2], &actions, nullptr, pointers.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      Require(error == 0, "CLI spawn failed");
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      Require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "CLI operation failed");
      std::ifstream output(output_path);
      const std::string text((std::istreambuf_iterator<char>(output)), {});
      Require(text == expected, "unexpected CLI output");
    };
    cli({"put", "cli-key", "cli-value"}, "OK\n");
    cli({"get", "cli-key"}, "cli-value\n");
    cli({"write", "put", "cli-batch", "yes", "delete", "cli-key"}, "OK\n");
    const auto initial_commit = client.Info(first).commit();
    Wait([&] {
      for (unsigned id = 1; id <= 3; ++id) if (client.Info(id).commit() < initial_commit) return false;
      return true;
    }, "initial replication did not reach all nodes");
    cluster.Kill(first);
    std::cout << "PASS: SIGKILL leader " << first << std::endl;
    Wait([&] { return cluster.Leader(client, first) != 0; }, "replacement election timed out");
    Value(client, "acknowledged", "before-crash");
    Value(client, "binary", std::string("a\0b", 3));
    Value(client, "empty", "");
    const auto missing = client.Get("deleted");
    Require(missing.status.IsNotFound(), ("deleted-key response: " + missing.ToString()).c_str());
    batch.Clear();
    batch.Put("after-switch", "still-safe");
    Require(client.Write(batch).ok(), "write after failover failed");
    cluster.Start(first);
    unsigned leader = cluster.Leader(client);
    const auto catchup_commit = client.Info(leader).commit();
    Wait([&] { return client.Info(first).commit() >= catchup_commit; }, "restarted node did not catch up");
    std::cout << "PASS: automatic failover and restarted replica catch-up" << std::endl;
    // A paused peer keeps sockets open, exercising connect/send/receive deadlines.
    leader = cluster.Leader(client);
    Require(leader != 0, "missing leader before pause");
    Require(::kill(cluster.pids[leader - 1], SIGSTOP) == 0, "SIGSTOP failed");
    Wait([&] { return cluster.Leader(client, leader) != 0; }, "election with paused old leader timed out");
    batch.Clear();
    batch.Put("after-pause", "new-leader");
    Require(client.Write(batch).ok(), "healthy majority blocked by paused old leader");
    Require(::kill(cluster.pids[leader - 1], SIGCONT) == 0, "SIGCONT failed");
    const auto new_leader = cluster.Leader(client, leader);
    const auto resume_commit = client.Info(new_leader).commit();
    Wait([&] { return client.Info(leader).commit() >= resume_commit && !client.Info(leader).is_leader(); },
         "resumed old leader did not step down and catch up");
    Value(client, "after-pause", "new-leader");
    std::cout << "PASS: paused old leader resumes safely" << std::endl;
    leader = cluster.Leader(client);
    for (unsigned id = 1; id <= 3; ++id) if (id != leader) cluster.Kill(id);
    batch.Clear();
    batch.Put("no-majority", "must-not-succeed");
    Require(!client.Write(batch).ok(), "minority acknowledged a write");
    Require(!client.Get("acknowledged").ok(), "minority served a successful stale read");
    std::cout << "PASS: one remaining node refuses successful reads/writes" << std::endl;
    cluster.Kill(leader);
    for (unsigned id = 1; id <= 3; ++id) cluster.Start(id);
    Wait([&] { return cluster.Leader(client) != 0; }, "full-restart election timed out");
    Value(client, "acknowledged", "before-crash");
    Value(client, "after-switch", "still-safe");
    Value(client, "after-pause", "new-leader");
    Require(client.Get("deleted").status.IsNotFound(), "delete lost after full restart");
    // Unknown writes may commit later. Never require their absence.
    std::cout << "PASS: all three SIGKILL/restart retain acknowledged data\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "; logs=" << cluster.root << '\n';
    return 1;
  }
}
