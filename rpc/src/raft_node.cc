#include "kv_raft/node.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include "protocol.h"
#include "storage_util.h"

namespace kv::raft {
using namespace std::chrono_literals;

Node::Node(Config config, unsigned id, std::filesystem::path root, Transport transport,
           std::function<Status(const char*)> before_io)
    : config_(std::move(config)), id_(id), root_(std::move(root)), store_(root_),
      transport_(std::move(transport)) { store_.before_io = std::move(before_io); }
Node::~Node() { Stop(); }
std::uint64_t Node::Last() const { return state_.snapshot_index() + state_.entries_size(); }
int Node::Offset(std::uint64_t index) const {
  return static_cast<int>(index - state_.snapshot_index() - 1);
}
std::uint64_t Node::TermAt(std::uint64_t index) const {
  return index == state_.snapshot_index() ? state_.snapshot_term() : state_.entries(Offset(index)).term();
}
void Node::Fail(Status status) {
  if (error_.ok()) {
    error_ = std::move(status);
    std::cerr << "node " << id_ << " fail-stop: " << error_.ToString() << '\n';
  }
  role_ = Role::Follower;
  leader_ = 0;
  cv_.notify_all();
}
bool Node::Persist() {
  auto status = store_.Save(state_);
  if (!status.ok()) Fail(status);
  return status.ok();
}
bool Node::Apply() {
  while (applied_ < state_.commit()) {
    const auto& entry = state_.entries(Offset(applied_ + 1));
    if (!entry.batch().empty()) {
      WriteBatch batch;
      auto status = batch.Decode(entry.batch());
      if (status.ok()) status = db_->Write(batch);
      if (!status.ok()) { Fail(status); return false; }
    }
    ++applied_;
  }
  cv_.notify_all();
  if (state_.ByteSizeLong() >= kSnapshotLogBytes ||
      state_.entries_size() >= static_cast<int>(kSnapshotLogEntries)) return Snapshot();
  return true;
}
bool Node::Snapshot() {
  if (applied_ == state_.snapshot_index()) return true;
  const auto index = applied_;
  const auto term = TermAt(index);
  std::uint64_t bytes = 0;
  auto status = store_.CreateSnapshot(*db_, index, term, &bytes);
  if (!status.ok()) { Fail(status); return false; }
  // No log prefix is removed until the snapshot file and its directory are synced.
  state_.mutable_entries()->DeleteSubrange(0, static_cast<int>(index - state_.snapshot_index()));
  state_.set_version(2);
  state_.set_snapshot_index(index);
  state_.set_snapshot_term(term);
  state_.set_snapshot_bytes(bytes);
  if (!Persist()) return false;
  status = store_.RemoveObsoleteSnapshots(state_);
  if (!status.ok()) { Fail(status); return false; }
  return true;
}
Status Node::TakeSnapshot() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || stopping_ || !error_.ok()) return Status::IOError("node unavailable");
  return Snapshot() ? Status::OK() : error_;
}
Status Node::RebuildDB() {
  if (db_) {
    auto status = db_->Close();
    if (!status.ok()) return status;
    db_.reset();
  }
  // data/ is a cache. A crash during rebuild is repaired from authoritative Raft files.
  std::error_code ec;
  std::filesystem::remove_all(root_ / "data", ec);
  if (ec) return Status::IOError("remove materialized data: " + ec.message());
  Options options;
  options.db_path = root_ / "data";
  db_ = std::make_unique<DB>(options);
  auto status = db_->Open();
  if (status.ok() && store_.before_io) status = store_.before_io("snapshot.restore");
  if (status.ok()) status = store_.RestoreSnapshot(state_, *db_);
  if (status.ok()) applied_ = state_.snapshot_index();
  return status;
}
Status Node::Open() {
  std::lock_guard<std::mutex> lock(mu_);
  if (db_) return Status::IOError("node already open");
  auto status = config_.Validate();
  if (!status.ok()) return status;
  if (id_ < 1 || id_ > 3) return Status::InvalidArgument("node ID must be 1, 2 or 3");
  status = store_.Open(config_.Identity(id_), &state_);
  if (!status.ok()) return status;
  for (const auto& entry : state_.entries()) {
    if (entry.batch().empty()) continue;
    WriteBatch batch;
    status = batch.Decode(entry.batch());
    if (status.ok()) status = rpc::detail::ValidateBatch(batch);
    if (!status.ok()) return Status::Corruption("invalid persisted Raft batch");
  }
  status = RebuildDB();
  if (!status.ok()) { Fail(status); return status; }
  if (!Apply()) return error_;
  ResetDeadline();
  return Status::OK();
}
void Node::ResetDeadline() {
  election_deadline_ = Clock::now() + std::chrono::milliseconds(
      std::uniform_int_distribution<int>(700, 1200)(random_));
}
void Node::ObserveTerm(std::uint64_t term) {
  if (term <= state_.term()) return;
  state_.set_term(term);
  state_.set_voted_for(0);
  role_ = Role::Follower;
  leader_ = 0;
  ResetDeadline();
  Persist();
  cv_.notify_all();
}
wire::VoteReply Node::Vote(const wire::VoteRequest& request) {
  std::lock_guard<std::mutex> lock(mu_);
  wire::VoteReply reply;
  reply.set_term(state_.term());
  if (stopping_ || !db_ || !error_.ok() || request.cluster() != config_.Identity(0) ||
      request.candidate() < 1 || request.candidate() > 3 || request.candidate() == id_ ||
      request.last_term() > request.term() ||
      (request.last_index() == 0) != (request.last_term() == 0)) return reply;
  ObserveTerm(request.term());
  reply.set_term(state_.term());
  if (!error_.ok() || request.term() != state_.term() || request.term() == 0) return reply;
  const bool up_to_date = request.last_term() > TermAt(Last()) ||
      (request.last_term() == TermAt(Last()) && request.last_index() >= Last());
  if (up_to_date && (state_.voted_for() == 0 || state_.voted_for() == request.candidate())) {
    state_.set_voted_for(request.candidate());
    if (Persist()) {
      reply.set_granted(true);
      ResetDeadline();
    }
  }
  return reply;
}
wire::AppendReply Node::Append(const wire::AppendRequest& request) {
  std::lock_guard<std::mutex> lock(mu_);
  wire::AppendReply reply;
  reply.set_term(state_.term());
  if (stopping_ || !db_ || !error_.ok() || request.cluster() != config_.Identity(0) ||
      request.leader() < 1 || request.leader() > 3 || request.leader() == id_ ||
      request.term() == 0 || request.entries_size() > 1 ||
      request.prev_index() == std::numeric_limits<std::uint64_t>::max() ||
      request.prev_term() > request.term() ||
      (request.prev_index() == 0) != (request.prev_term() == 0)) return reply;
  for (const auto& entry : request.entries()) {
    if (entry.term() == 0 || entry.term() < request.prev_term() || entry.term() > request.term() ||
        entry.batch().size() > rpc::kMaxPayloadBytes + 65536) return reply;
    if (!entry.batch().empty()) {
      WriteBatch batch;
      auto status = batch.Decode(entry.batch());
      if (!status.ok() || !rpc::detail::ValidateBatch(batch).ok()) return reply;
    }
  }
  ObserveTerm(request.term());
  reply.set_term(state_.term());
  if (!error_.ok() || request.term() != state_.term()) return reply;
  role_ = Role::Follower;
  leader_ = request.leader();
  ResetDeadline();
  cv_.notify_all();
  if (request.prev_index() < state_.snapshot_index()) {
    reply.set_next_index(state_.snapshot_index() + 1);
    return reply;
  }
  if (request.prev_index() > Last()) { reply.set_next_index(Last() + 1); return reply; }
  if (TermAt(request.prev_index()) != request.prev_term()) return reply;
  bool changed = false;
  if (request.entries_size() == 1) {
    const auto index = request.prev_index() + 1;
    const auto& entry = request.entries(0);
    if (index <= Last() && TermAt(index) != entry.term()) {
      if (index <= state_.commit()) { Fail(Status::Corruption("attempt to overwrite committed entry")); return reply; }
      state_.mutable_entries()->DeleteSubrange(Offset(index),
                                               static_cast<int>(Last() - index + 1));
      changed = true;
    }
    if (index > Last()) { *state_.add_entries() = entry; changed = true; }
    else if (state_.entries(Offset(index)).batch() != entry.batch()) {
      Fail(Status::Corruption("same index/term with different command"));
      return reply;
    }
  }
  const auto matched = request.prev_index() + static_cast<std::uint64_t>(request.entries_size());
  const auto commit = std::min(request.commit(), matched);
  if (commit > state_.commit()) { state_.set_commit(commit); changed = true; }
  if (changed && !Persist()) return reply;
  if (!Apply()) return reply;
  reply.set_success(true);
  reply.set_matched(matched);
  return reply;
}
wire::InstallReply Node::Install(const wire::InstallRequest& request) {
  std::lock_guard<std::mutex> lock(mu_);
  wire::InstallReply reply;
  reply.set_term(state_.term());
  if (stopping_ || !db_ || !error_.ok() || request.cluster() != config_.Identity(0) ||
      request.leader() < 1 || request.leader() > 3 || request.leader() == id_ || request.term() == 0 ||
      request.index() == 0 || request.index() == std::numeric_limits<std::uint64_t>::max() ||
      request.snapshot_term() == 0 || request.snapshot_term() > request.term() ||
      request.total_bytes() < 40 || request.total_bytes() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      request.offset() > request.total_bytes() || request.data().empty() ||
      request.data().size() > kSnapshotChunkBytes ||
      request.data().size() > request.total_bytes() - request.offset() ||
      CRC32(request.data()) != request.crc()) return reply;
  ObserveTerm(request.term());
  reply.set_term(state_.term());
  if (!error_.ok() || request.term() != state_.term()) return reply;
  role_ = Role::Follower;
  leader_ = request.leader();
  ResetDeadline();
  cv_.notify_all();
  if (request.index() <= state_.commit()) {
    // A delayed or duplicate snapshot cannot roll back committed/applied data.
    reply.set_success(true);
    reply.set_installed_index(request.index());
    return reply;
  }
  std::uint64_t next = 0;
  auto status = store_.ReceiveSnapshot(request, &next);
  if (status.IsInvalidArgument()) return reply;  // Restart an interrupted/out-of-order stream.
  if (!status.ok()) { Fail(status); return reply; }
  if (next == request.total_bytes()) {
    const bool keep_suffix = request.index() <= Last() && TermAt(request.index()) == request.snapshot_term();
    if (keep_suffix)
      state_.mutable_entries()->DeleteSubrange(0, static_cast<int>(request.index() - state_.snapshot_index()));
    else state_.clear_entries();
    state_.set_version(2);
    state_.set_snapshot_index(request.index());
    state_.set_snapshot_term(request.snapshot_term());
    state_.set_snapshot_bytes(request.total_bytes());
    state_.set_commit(request.index());
    if (!Persist()) return reply;
    status = RebuildDB();
    if (status.ok()) status = store_.RemoveObsoleteSnapshots(state_);
    if (!status.ok()) { Fail(status); return reply; }
    reply.set_installed_index(request.index());
  }
  reply.set_success(true);
  reply.set_next_offset(next);
  return reply;
}
void Node::WinElection() {
  role_ = Role::Leader;
  leader_ = id_;
  next_.fill(Last() + 1);
  match_.fill(0);
  // Current-term no-op commits entries inherited from earlier leaders.
  if (Last() == std::numeric_limits<std::uint64_t>::max()) {
    Fail(Status::Corruption("log index exhausted"));
    return;
  }
  state_.add_entries()->set_term(state_.term());
  if (!Persist()) return;
  match_[id_ - 1] = Last();
  std::cout << "node " << id_ << " leader term " << state_.term() << std::endl;
  cv_.notify_all();
}
void Node::AdvanceCommit() {
  auto matched = match_;
  std::sort(matched.begin(), matched.end());
  const auto majority = matched[1];
  if (majority > state_.commit() && TermAt(majority) == state_.term()) {
    state_.set_commit(majority);
    if (Persist()) Apply();
  }
}
void Node::TimerLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    cv_.wait_for(lock, 20ms);
    if (stopping_ || !error_.ok() || role_ == Role::Leader || Clock::now() < election_deadline_) continue;
    if (state_.term() == std::numeric_limits<std::uint64_t>::max()) {
      Fail(Status::Corruption("term exhausted"));
      continue;
    }
    state_.set_term(state_.term() + 1);
    state_.set_voted_for(id_);
    leader_ = 0;
    role_ = Role::Candidate;
    votes_.fill(false);
    votes_[id_ - 1] = true;
    ResetDeadline();
    Persist();
    cv_.notify_all();
  }
}
void Node::PeerLoop(unsigned peer) {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    cv_.wait_for(lock, 35ms);
    if (stopping_) return;
    if (!error_.ok() || role_ == Role::Follower) continue;
    const auto term = state_.term();
    const bool voting = role_ == Role::Candidate;
    std::string payload;
    std::uint64_t sent_next = 0, snapshot_index = 0, snapshot_end = 0;
    bool installing = false;
    if (voting) {
      if (votes_[peer - 1]) continue;
      wire::VoteRequest request;
      request.set_cluster(config_.Identity(0));
      request.set_term(term);
      request.set_candidate(id_);
      request.set_last_index(Last());
      request.set_last_term(TermAt(Last()));
      request.SerializeToString(&payload);
    } else {
      sent_next = next_[peer - 1];
      if (sent_next <= state_.snapshot_index()) {
        installing = true;
        snapshot_index = state_.snapshot_index();
        if (sending_snapshot_[peer - 1] != snapshot_index) {
          sending_snapshot_[peer - 1] = snapshot_index;
          sending_offset_[peer - 1] = 0;
        }
        wire::InstallRequest request;
        request.set_cluster(config_.Identity(0));
        request.set_leader(id_);
        request.set_term(term);
        request.set_index(snapshot_index);
        request.set_snapshot_term(state_.snapshot_term());
        request.set_total_bytes(state_.snapshot_bytes());
        request.set_offset(sending_offset_[peer - 1]);
        auto status = store_.ReadSnapshot(state_, request.offset(), request.mutable_data());
        if (!status.ok()) { Fail(status); continue; }
        request.set_crc(CRC32(request.data()));
        snapshot_end = request.offset() + request.data().size();
        request.SerializeToString(&payload);
      } else {
        wire::AppendRequest request;
        request.set_cluster(config_.Identity(0));
        request.set_term(term);
        request.set_leader(id_);
        request.set_prev_index(sent_next - 1);
        request.set_prev_term(TermAt(sent_next - 1));
        request.set_commit(state_.commit());
        if (sent_next <= Last()) *request.add_entries() = state_.entries(Offset(sent_next));
        request.SerializeToString(&payload);
      }
    }
    const char* method = voting ? "Vote" : installing ? "Install" : "Append";
    lock.unlock();
    const auto response = transport_ ? transport_(peer, method, payload)
        : Exchange(config_.peers[peer - 1], "Raft", method, payload, 250ms);
    lock.lock();
    if (stopping_ || !error_.ok() || response.status_code != 0) continue;
    if (voting) {
      wire::VoteReply reply;
      if (!reply.ParseFromString(response.payload)) continue;
      ObserveTerm(reply.term());
      if (!error_.ok() || role_ != Role::Candidate || state_.term() != term || reply.term() != term) continue;
      if (reply.granted()) votes_[peer - 1] = true;
      if (std::count(votes_.begin(), votes_.end(), true) >= 2) WinElection();
    } else if (installing) {
      wire::InstallReply reply;
      if (!reply.ParseFromString(response.payload)) continue;
      ObserveTerm(reply.term());
      if (!error_.ok() || role_ != Role::Leader || state_.term() != term || reply.term() != term ||
          snapshot_index != state_.snapshot_index()) continue;
      if (reply.success() && reply.installed_index() == snapshot_index) {
        match_[peer - 1] = std::max(match_[peer - 1], snapshot_index);
        next_[peer - 1] = match_[peer - 1] + 1;
        sending_offset_[peer - 1] = 0;
        AdvanceCommit();
      } else if (reply.success() && reply.next_offset() == snapshot_end && snapshot_end < state_.snapshot_bytes()) {
        sending_offset_[peer - 1] = snapshot_end;
      } else {
        sending_offset_[peer - 1] = 0;
      }
    } else {
      wire::AppendReply reply;
      if (!reply.ParseFromString(response.payload)) continue;
      ObserveTerm(reply.term());
      if (!error_.ok() || role_ != Role::Leader || state_.term() != term || reply.term() != term) continue;
      if (reply.success()) {
        // One outstanding call per peer: reject acknowledgements outside the sent range.
        if (reply.matched() < sent_next - 1 || reply.matched() > sent_next || reply.matched() > Last()) continue;
        match_[peer - 1] = std::max(match_[peer - 1], reply.matched());
        next_[peer - 1] = match_[peer - 1] + 1;
        AdvanceCommit();
      } else if (next_[peer - 1] > 1) {
        const auto hint = reply.next_index();
        if (hint > 0 && hint <= Last() + 1 && hint != next_[peer - 1]) next_[peer - 1] = hint;
        else --next_[peer - 1];
      }
    }
  }
}
void Node::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_ || stopping_ || !db_ || !error_.ok()) return;
  started_ = true;
  workers_[id_ - 1] = std::thread([this] { TimerLoop(); });
  for (unsigned peer = 1; peer <= 3; ++peer)
    if (peer != id_) workers_[peer - 1] = std::thread([this, peer] { PeerLoop(peer); });
}
void Node::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
    cv_.notify_all();
  }
  for (auto& worker : workers_) if (worker.joinable()) worker.join();
}
wire::Info Node::Info() {
  std::lock_guard<std::mutex> lock(mu_);
  wire::Info info;
  info.set_id(id_);
  info.set_leader(leader_);
  info.set_is_leader(!stopping_ && error_.ok() && role_ == Role::Leader);
  info.set_term(state_.term());
  info.set_commit(state_.commit());
  info.set_last_index(Last());
  info.set_snapshot_index(state_.snapshot_index());
  info.set_applied_index(applied_);
  if (!error_.ok()) info.set_error(error_.ToString());
  return info;
}
rpc::Result Node::Execute(const WriteBatch* batch, const std::string& key) {
  rpc::Result result;
  result.commit_state = batch ? rpc::CommitState::NotCommitted : rpc::CommitState::NotApplicable;
  if (batch) {
    auto status = rpc::detail::ValidateBatch(*batch);
    if (!status.ok()) { result.status = status; return result; }
  } else {
    auto status = rpc::detail::ValidateKey(key);
    if (!status.ok()) { result.status = status; return result; }
  }
  // ponytail: one business request at a time; batch/pipeline if throughput warrants it.
  std::lock_guard<std::mutex> serial(request_mu_);
  std::unique_lock<std::mutex> lock(mu_);
  if (stopping_ || !error_.ok() || role_ != Role::Leader) {
    result.status = Status::IOError("not an available leader");
    return result;
  }
  const auto term = state_.term();
  if (Last() == std::numeric_limits<std::uint64_t>::max()) {
    Fail(Status::Corruption("log index exhausted"));
    result.status = error_;
    return result;
  }
  const auto index = Last() + 1;
  auto* entry = state_.add_entries();
  entry->set_term(term);
  if (batch) entry->set_batch(batch->Encode());
  // A read's current-term replicated no-op is a clock-independent quorum barrier.
  if (batch) result.commit_state = rpc::CommitState::Unknown;
  if (!Persist()) { result.status = error_; return result; }
  match_[id_ - 1] = Last();
  cv_.notify_all();
  const bool finished = cv_.wait_for(lock, 1500ms, [&] {
    return applied_ >= index || stopping_ || !error_.ok() || role_ != Role::Leader || state_.term() != term;
  });
  if (!finished || stopping_ || !error_.ok() || role_ != Role::Leader || state_.term() != term || applied_ < index) {
    result.status = Status::IOError("quorum unavailable or leadership changed; write may have committed");
    return result;
  }
  if (batch) {
    result.status = Status::OK();
    result.commit_state = rpc::CommitState::Committed;
  } else {
    result.status = db_->Get(key, &result.value);
    if (result.status.ok() && result.value.size() > rpc::kMaxValueBytes)
      result.status = Status::InvalidArgument("stored value exceeds RPC limit");
    if (!result.status.ok()) result.value.clear();
  }
  return result;
}
}  // namespace kv::raft
