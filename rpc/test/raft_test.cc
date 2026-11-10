#include <fstream>
#include <atomic>
#include "kv_raft/client.h"
#include "minirpc/server/rpc_server.h"
#include "kv_raft/node.h"
#include "test_support.h"

using namespace kv::raft;


namespace {
Config TestConfig() {
  Config config;
  config.cluster = "safety-test";
  for (unsigned i = 0; i < 3; ++i) {
    config.clients[i] = {"127.0.0.1", static_cast<std::uint16_t>(21000 + i)};
    config.peers[i] = {"127.0.0.1", static_cast<std::uint16_t>(21100 + i)};
  }
  return config;
}
template <class Predicate>
void Wait(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(false && "timed out waiting for consensus");
}
void StoreFaults() {
  for (const std::string point : {"write", "fsync", "rename", "directory"}) {
    const auto root = TestDBPath("raft_fault_" + point);
    std::filesystem::remove_all(root);
    {
      Store store(root);
      wire::State state;
      MustOK(store.Open("fault-test", &state));
      state.set_term(1);
      MustOK(store.Save(state));
      state.set_term(2);
      store.before_io = [&](const char* phase) {
        return point == phase ? kv::Status::IOError("injected " + point) : kv::Status::OK();
      };
      CHECK(!store.Save(state).ok());
    }
    Store reopened(root);
    wire::State state;
    MustOK(reopened.Open("fault-test", &state));
    CHECK(state.term() == (point == "directory" ? 2 : 1));
  }
  const auto root = TestDBPath("raft_fail_stop");
  std::filesystem::remove_all(root);
  Node node(TestConfig(), 1, root);
  MustOK(node.Open());
  std::filesystem::create_directory(root / "raft.tmp");  // Real open/write failure.
  wire::VoteRequest vote;
  vote.set_cluster(TestConfig().Identity(0));
  vote.set_candidate(2);
  vote.set_term(1);
  CHECK(!node.Vote(vote).granted());
  CHECK(!node.Info().error().empty());
  CHECK(!node.Vote(vote).granted());
  std::cout << "PASS: write/fsync/rename/directory faults and fail-stop\n";
}
void LogMatching() {
  const auto root = TestDBPath("raft_log_matching");
  std::filesystem::remove_all(root);
  Node node(TestConfig(), 1, root);
  MustOK(node.Open());
  wire::AppendRequest request;
  request.set_cluster(TestConfig().Identity(0));
  request.set_leader(2);
  request.set_term(1);
  request.add_entries()->set_term(1);
  CHECK(node.Append(request).success());
  request.set_term(2);
  request.mutable_entries(0)->set_term(2);
  CHECK(node.Append(request).success());  // Uncommitted tail can be replaced.
  request.clear_entries();
  request.set_prev_index(1);
  request.set_prev_term(2);
  request.set_commit(1);
  CHECK(node.Append(request).success());
  request.set_prev_index(0);
  request.set_prev_term(0);
  request.set_commit(99);
  CHECK(node.Append(request).success());  // Heartbeat cannot commit beyond its prefix.
  CHECK(node.Info().commit() == 1 && node.Info().last_index() == 1);
  request.set_term(3);
  request.add_entries()->set_term(3);
  CHECK(!node.Append(request).success());  // Never overwrite a committed prefix.
  CHECK(!node.Info().error().empty());
  std::cout << "PASS: conflicting tail replacement and committed-prefix protection\n";
}
void ReplayAheadOfKV() {
  const auto root = TestDBPath("raft_replay_boundary");
  std::filesystem::remove_all(root);
  const auto config = TestConfig();
  {
    Store store(root);
    wire::State state;
    MustOK(store.Open(config.Identity(1), &state));
    kv::WriteBatch batch;
    batch.Put("overwritten", "old");
    batch.Put("deleted", "old");
    auto* entry = state.add_entries();
    entry->set_term(1);
    entry->set_batch(batch.Encode());
    kv::DB db(root / "data");
    MustOK(db.Open());
    MustOK(db.Write(batch));  // Only the first committed batch reached KV.
    batch.Clear();
    batch.Put("overwritten", "new");
    entry = state.add_entries();
    entry->set_term(2);
    entry->set_batch(batch.Encode());
    batch.Clear();
    batch.Delete("deleted");
    entry = state.add_entries();
    entry->set_term(2);
    entry->set_batch(batch.Encode());
    state.set_term(2);
    state.set_commit(3);
    MustOK(store.Save(state));  // Crash-equivalent state: commit ahead of apply.
  }
  for (int restart = 0; restart < 2; ++restart) {
    {
      Node node(config, 1, root);
      MustOK(node.Open());
      CHECK(node.Info().commit() == 3);
    }
    kv::DB db(root / "data");
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get("overwritten", &value));
    CHECK(value == "new");
    CHECK(db.Get("deleted", &value).IsNotFound());
  }
  std::cout << "PASS: replay committed history ahead of KV, including repeated overwrite/delete replay\n";
}
void UnknownIsNotRetried() {
  auto config = TestConfig();
  minirpc::RpcServer first, second;
  std::atomic<unsigned> mutations{0};
  std::atomic<bool> promote_second{false}, delay{true};
  auto info_handler = [&](unsigned id) {
    return [&, id](const minirpc::RpcRequest& request) {
      wire::Info info;
      info.set_id(id);
      info.set_is_leader(id == 1 || promote_second.load());
      info.set_term(id == 2 && promote_second.load() ? 2 : 1);
      minirpc::RpcResponse response;
      response.request_id = request.request_id;
      response.payload = info.SerializeAsString();
      return response;
    };
  };
  first.RegisterService("ClusterKV", "Info", info_handler(1));
  second.RegisterService("ClusterKV", "Info", info_handler(2));
  auto mutation = [&](const minirpc::RpcRequest& request) {
    ++mutations;
    promote_second.store(true);
    if (delay.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2400));
    wire::ClientReply body;
    auto* result = body.mutable_result();
    result->set_version(1);
    result->set_code(kv::rpc::wire::Reply::IO_ERROR);
    result->set_commit_state(kv::rpc::wire::Reply::UNKNOWN);
    minirpc::RpcResponse response;
    response.request_id = request.request_id;
    response.payload = body.SerializeAsString();
    return response;
  };
  first.RegisterService("ClusterKV", "Write", mutation);
  second.RegisterService("ClusterKV", "Write", mutation);
  CHECK(first.Start(config.clients[0]).ok());
  CHECK(second.Start(config.clients[1]).ok());
  Client client(config);
  kv::WriteBatch batch;
  batch.Put("unknown", "value");
  auto result = client.Write(batch);
  CHECK(!result.ok() && result.commit_state == kv::rpc::CommitState::Unknown);
  CHECK(mutations.load() == 1);  // Timed-out response must not trigger another write.
  first.Stop();
  delay.store(false);
  result = client.Write(batch);
  CHECK(!result.ok() && result.commit_state == kv::rpc::CommitState::Unknown);
  CHECK(mutations.load() == 2);  // Explicit UNKNOWN must not trigger another write either.
  second.Stop();
  std::cout << "PASS: neither transport timeout nor UNKNOWN replays mutations\n";
}
void PartitionSafety() {
  const auto root = TestDBPath("raft_partition");
  std::filesystem::remove_all(root);
  std::filesystem::create_directory(root);
  auto config = TestConfig();
  std::array<std::unique_ptr<Node>, 3> nodes;
  std::atomic<unsigned> isolated{0}, withhold_commit_to{0};
  std::atomic<std::uint64_t> commit_ceiling{0};
  for (unsigned id = 1; id <= 3; ++id) {
    auto transport = [&, id](unsigned peer, const char* method, const std::string& payload) {
      minirpc::RpcResponse reply;
      if (isolated.load() == id || isolated.load() == peer) {
        reply.status_code = 1;
        return reply;
      }
      if (std::string(method) == "Vote") {
        wire::VoteRequest request;
        CHECK(request.ParseFromString(payload));
        nodes[peer - 1]->Vote(request).SerializeToString(&reply.payload);
      } else {
        wire::AppendRequest request;
        CHECK(request.ParseFromString(payload));
        if (peer == withhold_commit_to.load() && request.commit() > commit_ceiling.load()) {
          reply.status_code = 1;  // Drop commit notifications, not the earlier durable append.
          return reply;
        }
        nodes[peer - 1]->Append(request).SerializeToString(&reply.payload);
      }
      return reply;
    };
    nodes[id - 1] = std::make_unique<Node>(config, id, root / std::to_string(id), transport);
    MustOK(nodes[id - 1]->Open());
  }
  for (auto& node : nodes) node->Start();
  auto leader = [&](unsigned excluded) {
    for (unsigned id = 1; id <= 3; ++id)
      if (id != excluded && nodes[id - 1]->Info().is_leader()) return id;
    return 0u;
  };
  unsigned old = 0, replacement = 0;
  Wait([&] { old = leader(0); return old != 0; });
  Wait([&] {
    const auto committed = nodes[old - 1]->Info().commit();
    if (committed == 0) return false;
    for (auto& node : nodes) if (node->Info().commit() != committed) return false;
    return true;
  });
  const auto follower = old % 3 + 1;
  const auto stale = 6 - old - follower;
  isolated.store(stale);
  commit_ceiling.store(nodes[follower - 1]->Info().commit());
  withhold_commit_to.store(follower);
  kv::WriteBatch batch;
  batch.Put("safe", "acknowledged");
  CHECK(nodes[old - 1]->Execute(&batch).ok());
  CHECK(nodes[follower - 1]->Info().commit() == commit_ceiling.load());
  CHECK(nodes[stale - 1]->Info().last_index() < nodes[old - 1]->Info().commit());
  // Lose the leader while the only other durable copy does not know it committed.
  isolated.store(old);
  withhold_commit_to.store(0);
  batch.Clear();
  batch.Put("safe", "minority-value");
  auto unknown = nodes[old - 1]->Execute(&batch);
  CHECK(!unknown.ok() && unknown.commit_state == kv::rpc::CommitState::Unknown);
  Wait([&] { replacement = leader(old); return replacement != 0; });
  CHECK(replacement == follower);  // Stale C cannot win against durable B.
  auto inherited = nodes[replacement - 1]->Execute(nullptr, "safe");
  CHECK(inherited.ok() && inherited.value == "acknowledged");
  CHECK(!nodes[old - 1]->Execute(nullptr, "safe").ok());
  batch.Clear();
  batch.Put("safe", "majority-value");
  CHECK(nodes[replacement - 1]->Execute(&batch).ok());
  auto value = nodes[replacement - 1]->Execute(nullptr, "safe");
  CHECK(value.ok() && value.value == "majority-value");
  const auto committed = nodes[replacement - 1]->Info().commit();
  isolated.store(0);
  Wait([&] { return !nodes[old - 1]->Info().is_leader() && nodes[old - 1]->Info().commit() >= committed; });
  for (auto& node : nodes) node->Stop();
  for (auto& node : nodes) node.reset();
  // Check each materialized replica after release of its exclusive DB lock.
  for (unsigned id = 1; id <= 3; ++id) {
    kv::DB db(root / std::to_string(id) / "data");
    MustOK(db.Open());
    std::string stored;
    MustOK(db.Get("safe", &stored));
    CHECK(stored == "majority-value");
  }
  std::cout << "PASS: isolated live old leader refuses success, majority wins, divergent tail repaired\n";
}
}
int main() {
  StoreFaults();
  LogMatching();
  ReplayAheadOfKV();
  UnknownIsNotRetried();
  PartitionSafety();
  const auto root = TestDBPath("raft_safety");
  std::filesystem::remove_all(root);
  Config config;
  config.cluster = "safety-test";
  for (int i = 0; i < 3; ++i) {
    config.clients[i] = {"127.0.0.1", static_cast<std::uint16_t>(21000 + i)};
    config.peers[i] = {"127.0.0.1", static_cast<std::uint16_t>(21100 + i)};
  }
  {
    Node node(config, 1, root);
    MustOK(node.Open());
    Node duplicate(config, 1, root);
    CHECK(!duplicate.Open().ok());
    wire::VoteRequest vote;
    vote.set_cluster(config.Identity(0));
    vote.set_candidate(2);
    vote.set_term(4);
    CHECK(node.Vote(vote).granted());
    vote.set_candidate(3);
    CHECK(!node.Vote(vote).granted());
  }
  {
    Node node(config, 1, root);
    MustOK(node.Open());
    wire::VoteRequest vote;
    vote.set_cluster(config.Identity(0));
    vote.set_candidate(3);
    vote.set_term(4);
    CHECK(!node.Vote(vote).granted());  // Vote survives process restart.
    wire::AppendRequest append;
    append.set_cluster(config.Identity(0));
    append.set_leader(2);
    append.set_term(5);
    auto* entry = append.add_entries();
    entry->set_term(5);
    kv::WriteBatch batch;
    batch.Put("safe", "value");
    entry->set_batch(batch.Encode());
    CHECK(node.Append(append).success());
    append.clear_entries();
    append.set_prev_index(1);
    append.set_prev_term(5);
    append.set_commit(1);
    CHECK(node.Append(append).success());
    append.set_term(4);
    CHECK(!node.Append(append).success());  // Stale leader cannot roll back data.
  }
  {
    Node node(config, 1, root);
    MustOK(node.Open());
    CHECK(node.Info().commit() == 1);
    wire::VoteRequest vote;
    vote.set_cluster(config.Identity(0));
    vote.set_candidate(3);
    vote.set_term(6);
    CHECK(!node.Vote(vote).granted());  // Outdated candidate cannot replace log.
  }
  {
    Config other = config;
    other.cluster = "different-cluster";
    Node node(other, 1, root);
    CHECK(!node.Open().ok());
  }
  { // CRC failure must not become an empty, voting node.
    std::fstream file(root / "raft.state", std::ios::binary | std::ios::in | std::ios::out);
    CHECK(file.is_open());
    file.seekp(12);
    file.put('\xff');
  }
  Node corrupt(config, 1, root);
  CHECK(!corrupt.Open().ok());
  std::cout << "Raft vote/recovery/identity/corruption checks passed\n";
}
