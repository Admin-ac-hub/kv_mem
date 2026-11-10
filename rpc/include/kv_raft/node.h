#pragma once
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include "db.h"
#include "kv_raft/store.h"
#include "kv_raft/transport.h"
#include "kv_rpc/client.h"

namespace kv::raft {

class Node {
 public:
  using Transport = std::function<minirpc::RpcResponse(unsigned, const char*, const std::string&)>;
  Node(Config config, unsigned id, std::filesystem::path root, Transport transport = {},
       std::function<Status(const char*)> before_io = {});
  ~Node();
  Status Open();
  void Start();
  void Stop();
  wire::VoteReply Vote(const wire::VoteRequest& request);
  wire::AppendReply Append(const wire::AppendRequest& request);
  wire::InstallReply Install(const wire::InstallRequest& request);
  Status TakeSnapshot();
  wire::Info Info();
  rpc::Result Execute(const WriteBatch* batch, const std::string& key = {});
 private:
  enum class Role { Follower, Candidate, Leader };
  using Clock = std::chrono::steady_clock;
  std::uint64_t Last() const;
  std::uint64_t TermAt(std::uint64_t index) const;
  bool Persist();
  bool Apply();
  bool Snapshot();
  Status RebuildDB();
  int Offset(std::uint64_t index) const;
  void Fail(Status status);
  void ResetDeadline();
  void ObserveTerm(std::uint64_t term);
  void WinElection();
  void AdvanceCommit();
  void TimerLoop();
  void PeerLoop(unsigned peer);

  Config config_;
  unsigned id_;
  std::filesystem::path root_;
  Store store_;
  Transport transport_;
  wire::State state_;
  std::unique_ptr<DB> db_;
  std::uint64_t applied_ = 0;
  Role role_ = Role::Follower;
  unsigned leader_ = 0;
  std::array<bool, 3> votes_{};
  std::array<std::uint64_t, 3> next_{};
  std::array<std::uint64_t, 3> match_{};
  std::array<std::uint64_t, 3> sending_snapshot_{}, sending_offset_{};
  Status error_;
  bool stopping_ = false;
  bool started_ = false;
  Clock::time_point election_deadline_;
  std::mt19937 random_{std::random_device{}()};
  std::mutex mu_;
  std::mutex request_mu_;
  std::condition_variable cv_;
  std::array<std::thread, 3> workers_;
};
}  // namespace kv::raft
