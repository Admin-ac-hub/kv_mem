#include <sys/wait.h>
#include <unistd.h>

#include <fstream>

#include "kv_raft/node.h"
#include "storage_util.h"
#include "test_support.h"

using namespace kv::raft;
namespace {
Config TestConfig() {
  Config config;
  config.cluster = "snapshot-test";
  for (unsigned i = 0; i < 3; ++i) {
    config.clients[i] = {"127.0.0.1", static_cast<std::uint16_t>(22000 + i)};
    config.peers[i] = {"127.0.0.1", static_cast<std::uint16_t>(22100 + i)};
  }
  return config;
}
void Append(Node& node, std::uint64_t index, const kv::WriteBatch& batch, bool committed = true,
            std::uint64_t term = 1, std::uint64_t previous_term = 1) {
  wire::AppendRequest request;
  request.set_cluster(TestConfig().Identity(0));
  request.set_leader(2);
  request.set_term(term);
  request.set_prev_index(index - 1);
  request.set_prev_term(index == 1 ? 0 : previous_term);
  request.set_commit(committed ? index : index - 1);
  auto* entry = request.add_entries();
  entry->set_term(term);
  entry->set_batch(batch.Encode());
  CHECK(node.Append(request).success());
}
void Seed(const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  Node node(TestConfig(), 1, root);
  MustOK(node.Open());
  kv::WriteBatch batch;
  batch.Put("overwritten", "old");
  batch.Put("deleted", "old");
  batch.Put("empty", "");
  batch.Put("binary", std::string("a\0b", 3));
  Append(node, 1, batch);
  MustOK(node.TakeSnapshot());
  batch.Clear();
  batch.Put("overwritten", "new");
  batch.Delete("deleted");
  // More than one transfer chunk; also tests a record crossing chunk boundaries.
  batch.Put("large", std::string(600000, 'L'));
  Append(node, 2, batch);
  batch.Clear();
  batch.Put("uncommitted", "must-not-appear");
  Append(node, 3, batch, false);
}
void CheckData(const std::filesystem::path& root, bool large = true) {
  kv::DB db(root / "data");
  MustOK(db.Open());
  std::string value;
  MustOK(db.Get("overwritten", &value));
  CHECK(value == "new");
  CHECK(db.Get("deleted", &value).IsNotFound());
  CHECK(db.Get("uncommitted", &value).IsNotFound());
  MustOK(db.Get("empty", &value));
  CHECK(value.empty());
  MustOK(db.Get("binary", &value));
  CHECK(value == std::string("a\0b", 3));
  if (large) {
    MustOK(db.Get("large", &value));
    CHECK(value == std::string(600000, 'L'));
  }
}
void WaitForCrash(pid_t child) {
  int status = 0;
  CHECK(child > 0);
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
}
void SnapshotFaults() {
  const std::vector<std::string> phases = {"snapshot.write",
                                           "snapshot.finish",
                                           "snapshot.fsync",
                                           "snapshot.rename",
                                           "snapshot.directory",
                                           "write",
                                           "fsync",
                                           "rename",
                                           "directory",
                                           "snapshot.cleanup",
                                           "snapshot.cleanup.directory"};
  for (bool crash : {false, true})
    for (const auto& phase : phases) {
      const auto root = TestDBPath("snapshot_fault_" + phase + (crash ? "_crash" : "_io"));
      Seed(root);
      auto run = [&] {
        bool armed = false, hit = false;
        Node node(TestConfig(), 1, root, {}, [&](const char* point) {
          if (armed && phase == point) {
            hit = true;
            if (crash) ::_exit(77);
            return kv::Status::IOError("injected " + phase);
          }
          return kv::Status::OK();
        });
        MustOK(node.Open());
        armed = true;
        CHECK(!node.TakeSnapshot().ok());
        CHECK(hit && !node.Info().error().empty());
      };
      if (crash) {
        std::cout.flush();
        const auto child = ::fork();
        if (child == 0) {
          run();
          ::_exit(1);
        }
        WaitForCrash(child);
      } else
        run();
      for (int restart = 0; restart < 2; ++restart) {
        {
          Node recovered(TestConfig(), 1, root);
          MustOK(recovered.Open());
          CHECK(recovered.Info().commit() == 2 && recovered.Info().last_index() == 3);
          CHECK(recovered.Info().applied_index() == 2);
          CHECK(recovered.Info().snapshot_index() ==
                ((phase == "directory" || phase == "snapshot.cleanup" ||
                  phase == "snapshot.cleanup.directory")
                     ? 2
                     : 1));
        }
        CheckData(root);
      }
    }
  std::cout << "PASS: generation, snapshot publication, log truncation and cleanup crash/I/O "
               "boundaries\n";
}
struct Source {
  std::filesystem::path root = TestDBPath("snapshot_source");
  Store store{root};
  wire::State state;
  Source() {
    Seed(root);
    {
      Node node(TestConfig(), 1, root);
      MustOK(node.Open());
      MustOK(node.TakeSnapshot());
    }
    MustOK(store.Open(TestConfig().Identity(1), &state));
    CHECK(state.snapshot_index() == 2 && state.entries_size() == 1);
  }
  wire::InstallRequest Chunk(std::uint64_t offset) {
    wire::InstallRequest request;
    request.set_cluster(TestConfig().Identity(0));
    request.set_leader(2);
    request.set_term(2);
    request.set_index(state.snapshot_index());
    request.set_snapshot_term(state.snapshot_term());
    request.set_total_bytes(state.snapshot_bytes());
    request.set_offset(offset);
    MustOK(store.ReadSnapshot(state, offset, request.mutable_data()));
    request.set_crc(kv::CRC32(request.data()));
    return request;
  }
  wire::InstallReply Install(Node& target, std::uint64_t offset = 0) {
    wire::InstallReply reply;
    while (offset < state.snapshot_bytes()) {
      const auto request = Chunk(offset);
      reply = target.Install(request);
      CHECK(reply.success());
      offset += request.data().size();
      CHECK(reply.next_offset() == offset || reply.installed_index() == state.snapshot_index());
    }
    CHECK(reply.installed_index() == 2);
    return reply;
  }
};
void InstallChecks() {
  Source source;
  const auto root = TestDBPath("snapshot_install");
  std::filesystem::remove_all(root);
  {
    Node node(TestConfig(), 1, root);
    MustOK(node.Open());
    kv::WriteBatch old;
    old.Put("discarded", "old materialized key");
    Append(node, 1, old);
    auto request = source.Chunk(kSnapshotChunkBytes);
    CHECK(!node.Install(request).success());  // Missing first chunk.
    request = source.Chunk(0);
    request.mutable_data()->at(0) ^= 1;
    CHECK(!node.Install(request).success());  // Bad per-chunk CRC.
    CHECK(node.Info().error().empty());
    request = source.Chunk(0);
    auto reply = node.Install(request);
    CHECK(reply.success() && reply.installed_index() == 0);
    CHECK(node.Info().snapshot_index() == 0 && node.Info().commit() == 1);
  }
  {
    Node node(TestConfig(), 1, root);
    MustOK(node.Open());
    CHECK(!node.Install(source.Chunk(kSnapshotChunkBytes))
               .success());  // Restart resets staging offset.
    source.Install(node);
    CHECK(node.Info().commit() == 2 && node.Info().applied_index() == 2);
    CHECK(node.Install(source.Chunk(0)).installed_index() ==
          2);  // Lost final ACK / stale snapshot.
    kv::WriteBatch tail;
    tail.Put("after-install", "tail");
    Append(node, 3, tail, true, 2, 1);
    CHECK(node.Install(source.Chunk(0)).installed_index() == 2);
    CHECK(node.Info().commit() == 3);
  }
  {
    Node node(TestConfig(), 1, root);
    MustOK(node.Open());
    CHECK(node.Info().commit() == 3);
  }
  CheckData(root);
  kv::DB db(root / "data");
  MustOK(db.Open());
  std::string value;
  CHECK(db.Get("discarded", &value).IsNotFound());
  MustOK(db.Get("after-install", &value));
  CHECK(value == "tail");
  std::cout << "PASS: chunk CRC/order, interrupted transfer, idempotent install, stale snapshot "
               "and clean materialization\n";
}
void InstallFaults() {
  Source source;
  for (const std::string phase :
       {"install.write", "snapshot.fsync", "snapshot.rename", "snapshot.directory", "write",
        "fsync", "rename", "directory", "snapshot.restore", "snapshot.restore.record",
        "snapshot.cleanup", "snapshot.cleanup.directory"}) {
    const auto root = TestDBPath("install_crash_" + phase);
    std::filesystem::remove_all(root);
    {
      Node node(TestConfig(), 1, root);
      MustOK(node.Open());
      kv::WriteBatch batch;
      batch.Put("old", "old");
      Append(node, 1, batch);
    }
    std::cout.flush();
    const auto child = ::fork();
    if (child == 0) {
      bool armed = false;
      Node node(TestConfig(), 1, root, {}, [&](const char* point) {
        if (armed && phase == point) ::_exit(77);
        return kv::Status::OK();
      });
      MustOK(node.Open());
      // Persist the new term before arming Raft-state I/O failure points.
      wire::VoteRequest vote;
      vote.set_cluster(TestConfig().Identity(0));
      vote.set_candidate(2);
      vote.set_term(2);
      vote.set_last_index(1);
      vote.set_last_term(1);
      CHECK(node.Vote(vote).granted());
      armed = true;
      source.Install(node);
      ::_exit(1);
    }
    WaitForCrash(child);
    const bool published = phase == "directory" || phase == "snapshot.restore" ||
                           phase == "snapshot.restore.record" || phase == "snapshot.cleanup" ||
                           phase == "snapshot.cleanup.directory";
    {
      Node node(TestConfig(), 1, root);
      MustOK(node.Open());
      CHECK(node.Info().commit() == (published ? 2 : 1));
      if (!published) source.Install(node);
    }
    CheckData(root);
  }
  std::cout << "PASS: install staging, publication and materialization crashes recover or retry\n";
}
void SuffixAndCorruption() {
  Source source;
  for (bool matching : {true, false}) {
    const auto root =
        TestDBPath(matching ? "snapshot_matching_suffix" : "snapshot_conflicting_suffix");
    Seed(root);
    {
      Node node(TestConfig(), 1, root);
      MustOK(node.Open());
      // Keep commit below incoming snapshot. A local matching suffix is safe to retain.
    }
    {
      Store store(root);
      wire::State state;
      MustOK(store.Open(TestConfig().Identity(1), &state));
      state.set_commit(1);
      if (!matching) {
        state.set_term(2);
        state.mutable_entries(0)->set_term(2);
        state.mutable_entries(1)->set_term(2);
      }
      MustOK(store.Save(state));
    }
    {
      Node node(TestConfig(), 1, root);
      MustOK(node.Open());
      source.Install(node);
      CHECK(node.Info().last_index() == (matching ? 3 : 2));
    }
    CheckData(root);
  }
  // A referenced snapshot must be present and intact. Never bootstrap an empty node.
  const auto root = TestDBPath("snapshot_corrupt");
  Seed(root);
  {
    Node node(TestConfig(), 1, root);
    MustOK(node.Open());
    MustOK(node.TakeSnapshot());
  }
  const auto path = root / "raft-2-1.snapshot";
  std::string bytes;
  {
    std::ifstream file(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(file), {});
  }
  for (const std::size_t offset : {std::size_t(10), std::size_t(48), bytes.size() - 1}) {
    auto corrupt = bytes;
    corrupt[offset] ^= 1;
    {
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      file.write(corrupt.data(), corrupt.size());
    }
    Node node(TestConfig(), 1, root);
    CHECK(!node.Open().ok());
  }
  std::filesystem::remove(path);
  Node missing(TestConfig(), 1, root);
  CHECK(!missing.Open().ok());
  std::cout << "PASS: suffix matching/discard, snapshot header/body/end corruption and "
               "missing-file rejection\n";
}
void SnapshotBounds() {
  const auto root = TestDBPath("snapshot_record_and_read_bounds");
  std::filesystem::remove_all(root);
  const std::string large_key(kv::rpc::kMaxPayloadBytes - 64, 'k');
  {
    Node node(TestConfig(), 1, root);
    MustOK(node.Open());
    kv::WriteBatch batch;
    batch.Put(large_key, "");
    Append(node, 1, batch);
    batch.Clear();
    batch.Put("z", std::string(kv::rpc::kMaxValueBytes, 'V'));
    Append(node, 2, batch);
    MustOK(node.TakeSnapshot());
    CHECK(node.Info().snapshot_index() == 2);
    batch.Clear();
    for (std::uint64_t index = 3; index <= kSnapshotLogEntries + 2; ++index)
      Append(node, index, batch);
    CHECK(node.Info().snapshot_index() == kSnapshotLogEntries + 2);
    wire::VoteRequest stale;
    stale.set_cluster(TestConfig().Identity(0));
    stale.set_candidate(3);
    stale.set_term(2);
    CHECK(!node.Vote(stale).granted());
  }
  {
    Node node(TestConfig(), 1, root);
    MustOK(node.Open());
    CHECK(node.Info().snapshot_index() == kSnapshotLogEntries + 2);
  }
  {
    kv::DB db(root / "data");
    MustOK(db.Open());
    std::string value;
    MustOK(db.Get(large_key, &value));
    CHECK(value.empty());
    MustOK(db.Get("z", &value));
    CHECK(value == std::string(kv::rpc::kMaxValueBytes, 'V'));
  }
  const auto empty_root = TestDBPath("snapshot_empty");
  std::filesystem::remove_all(empty_root);
  {
    Node node(TestConfig(), 1, empty_root);
    MustOK(node.Open());
    kv::WriteBatch batch;
    Append(node, 1, batch);
    MustOK(node.TakeSnapshot());
  }
  Store store(empty_root);
  wire::State state;
  MustOK(store.Open(TestConfig().Identity(1), &state));
  CHECK(state.snapshot_bytes() == 40 && state.entries_size() == 0);
  std::cout << "PASS: empty snapshot, maximum key/value records, no-op history compaction and "
               "snapshot voting boundary\n";
}

}  // namespace
int main() {
  SnapshotFaults();
  InstallChecks();
  InstallFaults();
  SuffixAndCorruption();
  SnapshotBounds();
}
