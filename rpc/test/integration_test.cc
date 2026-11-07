#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "db.h"
#include "kv_rpc/client.h"
#include "kv_rpc/service.h"
#include "kv_service.pb.h"

namespace {

using namespace std::chrono_literals;
using kv::rpc::CommitState;
using kv::rpc::wire::Operation;
using kv::rpc::wire::Reply;
using kv::rpc::wire::WriteRequest;

const minirpc::Endpoint kEndpoint{"127.0.0.1", 19879};

[[noreturn]] void CheckFailed(const char* expression, int line) {
  std::cerr << __FILE__ << ':' << line << ": CHECK failed: "
            << expression << '\n';
  std::abort();
}

#define CHECK(expression)                 \
  do {                                    \
    if (!(expression)) {                  \
      CheckFailed(#expression, __LINE__);  \
    }                                     \
  } while (false)

template <class Status>
void MustOK(const Status& status) {
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    std::abort();
  }
}

class TestDirectory {
 public:
  explicit TestDirectory(const char* name)
      : path(std::filesystem::temp_directory_path() /
             ("kv_rpc_" + std::to_string(::getpid()) + "_" + name)) {
    CHECK(!std::filesystem::exists(path));
  }
  ~TestDirectory() { std::filesystem::remove_all(path); }

  const std::filesystem::path path;
};

kv::Options WithPath(kv::Options options, const std::filesystem::path& path) {
  options.db_path = path;
  return options;
}

struct Fixture {
  explicit Fixture(const char* name, kv::Options options = {})
      : directory(name), db(WithPath(std::move(options), directory.path)) {
    MustOK(db.Open());
    kv::rpc::RegisterService(server, db);
    MustOK(server.Start(kEndpoint));
  }

  ~Fixture() { server.Stop(5s); }

  TestDirectory directory;
  kv::DB db;
  minirpc::RpcServer server;
};

Reply CallRaw(minirpc::RpcClient& client, const char* method,
              const std::string& payload) {
  const auto response = client.Call("KVService", method, payload, 5s);
  if (response.status_code != 0) {
    std::cerr << "RPC failed: " << response.error_message << '\n';
  }
  CHECK(response.status_code == 0);
  Reply reply;
  CHECK(reply.ParseFromString(response.payload));
  CHECK(reply.version() == 1);
  return reply;
}

WriteRequest MakeWrite(const std::string& key, const std::string& value) {
  WriteRequest request;
  request.set_version(1);
  auto* operation = request.add_operations();
  operation->set_type(Operation::PUT);
  operation->set_key(key);
  operation->set_value(value);
  return request;
}

void CheckRejected(const kv::rpc::Result& result) {
  CHECK(!result.ok());
  CHECK(result.transport_status.ok());
  CHECK(result.status.IsInvalidArgument());
  CHECK(result.commit_state == CommitState::NotCommitted);
}

void CheckRejected(const Reply& reply) {
  CHECK(reply.code() == Reply::INVALID_ARGUMENT);
  CHECK(reply.commit_state() == Reply::NOT_COMMITTED);
}

void CheckValue(kv::rpc::Client& client, const std::string& key,
                const std::string& expected) {
  const auto result = client.Get(key, 5s);
  MustOK(result);
  CHECK(result.commit_state == CommitState::NotApplicable);
  CHECK(result.value == expected);
}

void CheckMissing(kv::rpc::Client& client, const std::string& key) {
  const auto result = client.Get(key, 5s);
  CHECK(result.transport_status.ok());
  CHECK(result.status.IsNotFound());
  CHECK(result.commit_state == CommitState::NotApplicable);
  CHECK(result.value.empty());
}

void TestClientRejectsBeforeConnecting() {
  // No server exists here: local validation must not need a connection.
  kv::rpc::Client client(kEndpoint);
  CheckRejected(client.Put("", "value"));
  CheckRejected(client.Delete("bad\nkey"));
  const auto invalid_get = client.Get("");
  CHECK(invalid_get.transport_status.ok());
  CHECK(invalid_get.status.IsInvalidArgument());
  CHECK(invalid_get.commit_state == CommitState::NotApplicable);
  const auto expired = client.Put("never-sent", "value", 0ms);
  CHECK(expired.transport_status.code() == minirpc::StatusCode::kTimeout);
  CHECK(expired.commit_state == CommitState::NotCommitted);
  client.Close();
}

void TestBasicBatchAndReopen() {
  Fixture fixture("basic");
  kv::rpc::Client client(kEndpoint);
  CheckMissing(client, "absent");
  const std::string binary("a\0b\t\n\xff", 6);
  const auto put = client.Put("binary", binary);
  MustOK(put);
  CHECK(put.commit_state == CommitState::Committed);
  CheckValue(client, "binary", binary);
  MustOK(client.Put("empty", ""));
  CheckValue(client, "empty", "");
  MustOK(client.Put("remove", "old"));

  kv::WriteBatch batch;
  batch.Put("left", "first");
  batch.Put("right", "second");
  batch.Delete("remove");
  batch.Put("left", "final");
  const auto written = client.Write(batch);
  MustOK(written);
  CHECK(written.commit_state == CommitState::Committed);
  CheckValue(client, "left", "final");
  CheckValue(client, "right", "second");
  CheckMissing(client, "remove");

  const auto deleted = client.Delete("right");
  MustOK(deleted);
  CHECK(deleted.commit_state == CommitState::Committed);
  CheckMissing(client, "right");
  MustOK(client.Delete("already-absent"));
  MustOK(client.Write(kv::WriteBatch{}));

  client.Close();
  fixture.server.Stop(5s);
  MustOK(fixture.db.Close());
  MustOK(fixture.db.Open());
  MustOK(fixture.server.Start(kEndpoint));
  kv::rpc::Client reopened(kEndpoint);
  CheckValue(reopened, "binary", binary);
  CheckValue(reopened, "left", "final");
  CheckValue(reopened, "empty", "");
  CheckMissing(reopened, "right");
  reopened.Close();
  fixture.server.Stop(5s);
  MustOK(fixture.db.Close());
}

void TestInvalidRequestsAreAtomic() {
  Fixture fixture("validation");
  kv::rpc::Client client(kEndpoint);
  minirpc::RpcClient raw(kEndpoint);
  MustOK(client.Put("unchanged", "before"));

  for (const auto& key : {std::string{}, std::string("bad\tkey"),
                          std::string("bad\nkey"), std::string("bad\0key", 7)}) {
    kv::WriteBatch batch;
    batch.Put("unchanged", "after");
    batch.Put(key, "invalid");
    CheckRejected(client.Write(batch));
    CheckValue(client, "unchanged", "before");

    auto request = MakeWrite("unchanged", "after");
    auto* operation = request.add_operations();
    operation->set_type(Operation::PUT);
    operation->set_key(key);
    operation->set_value("invalid");
    CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
    CheckValue(client, "unchanged", "before");
  }

  CheckRejected(CallRaw(raw, "Write", std::string(1, '\x80')));
  auto request = MakeWrite("unchanged", "after");
  request.set_version(0);
  CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
  request.set_version(2);
  CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
  request.set_version(1);
  request.mutable_operations(0)->set_type(static_cast<Operation::Type>(99));
  CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
  request.mutable_operations(0)->set_type(Operation::DELETE);
  CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
  request.mutable_operations(0)->clear_value();
  CheckRejected(CallRaw(raw, "Put", request.SerializeAsString()));
  request.mutable_operations(0)->set_type(Operation::PUT);
  CheckRejected(CallRaw(raw, "Delete", request.SerializeAsString()));
  *request.add_operations() = request.operations(0);
  CheckRejected(CallRaw(raw, "Put", request.SerializeAsString()));
  CheckValue(client, "unchanged", "before");

  kv::rpc::wire::GetRequest get;
  get.set_version(2);
  get.set_key("unchanged");
  auto reply = CallRaw(raw, "Get", get.SerializeAsString());
  CHECK(reply.code() == Reply::INVALID_ARGUMENT);
  CHECK(reply.commit_state() == Reply::NOT_APPLICABLE);
  reply = CallRaw(raw, "Get", std::string(1, '\x80'));
  CHECK(reply.code() == Reply::INVALID_ARGUMENT);
  CHECK(reply.commit_state() == Reply::NOT_APPLICABLE);

  raw.Close();
  client.Close();
  fixture.server.Stop(5s);
  MustOK(fixture.db.Close());
}

void TestRequestLimits() {
  Fixture fixture("limits");
  kv::rpc::Client client(kEndpoint);
  minirpc::RpcClient raw(kEndpoint);

  const std::string maximum_value(kv::rpc::kMaxValueBytes, 'v');
  MustOK(client.Put("maximum", maximum_value, 5s));
  CheckValue(client, "maximum", maximum_value);
  const std::string too_large_value(kv::rpc::kMaxValueBytes + 1, 'v');
  CheckRejected(client.Put("oversized-value", too_large_value));
  auto request = MakeWrite("oversized-value", too_large_value);
  CHECK(request.ByteSizeLong() < kv::rpc::kMaxPayloadBytes);
  CheckRejected(CallRaw(raw, "Put", request.SerializeAsString()));
  CheckMissing(client, "oversized-value");
  MustOK(fixture.db.Put("local-oversized", too_large_value));
  const auto oversized_read = client.Get("local-oversized");
  CHECK(oversized_read.transport_status.ok());
  CHECK(oversized_read.status.IsInvalidArgument());
  CHECK(oversized_read.value.empty());

  kv::WriteBatch many;
  request.Clear();
  request.set_version(1);
  for (std::size_t i = 0; i <= kv::rpc::kMaxBatchOperations; ++i) {
    const auto key = "operation-" + std::to_string(i);
    many.Put(key, "value");
    auto* operation = request.add_operations();
    operation->set_type(Operation::PUT);
    operation->set_key(key);
    operation->set_value("value");
  }
  CheckRejected(client.Write(many));
  CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
  CheckMissing(client, "operation-0");
  request.mutable_operations()->RemoveLast();
  const auto maximum_batch = CallRaw(raw, "Write", request.SerializeAsString());
  CHECK(maximum_batch.code() == Reply::OK);
  CHECK(maximum_batch.commit_state() == Reply::COMMITTED);
  CheckValue(client, "operation-0", "value");
  CheckValue(client, "operation-" + std::to_string(kv::rpc::kMaxBatchOperations - 1),
             "value");

  // Raw keys/values fit, but their protobuf envelope crosses the payload limit.
  const std::string half_payload(kv::rpc::kMaxPayloadBytes / 2 - 16, 'p');
  kv::WriteBatch oversized;
  oversized.Put("payload-a", half_payload);
  oversized.Put("payload-b", half_payload);
  CheckRejected(client.Write(oversized));
  request = MakeWrite("payload-a", half_payload);
  auto* operation = request.add_operations();
  operation->set_type(Operation::PUT);
  operation->set_key("payload-b");
  operation->set_value(half_payload);
  CHECK(request.ByteSizeLong() > kv::rpc::kMaxPayloadBytes);
  CheckRejected(CallRaw(raw, "Write", request.SerializeAsString()));
  CheckMissing(client, "payload-a");
  CheckMissing(client, "payload-b");

  raw.Close();
  client.Close();
  fixture.server.Stop(5s);
  MustOK(fixture.db.Close());
}

void TestConcurrentClients() {
  Fixture fixture("concurrent");
  std::vector<std::future<void>> workers;
  for (int worker = 0; worker < 6; ++worker) {
    workers.push_back(std::async(std::launch::async, [worker] {
      kv::rpc::Client client(kEndpoint);
      for (int i = 0; i < 24; ++i) {
        const auto key = "worker-" + std::to_string(worker) + "-" + std::to_string(i);
        kv::WriteBatch batch;
        batch.Put(key, "value");
        batch.Put(key + "-pair", "paired");
        MustOK(client.Write(batch, 5s));
        CheckValue(client, key, "value");
        CheckValue(client, key + "-pair", "paired");
      }
      client.Close();
    }));
  }
  for (auto& worker : workers) {
    CHECK(worker.wait_for(15s) == std::future_status::ready);
    worker.get();
  }
  fixture.server.Stop(5s);
  MustOK(fixture.db.Close());
}

struct WriteGate {
  WriteGate() : entered(entered_signal.get_future()), release(release_signal.get_future().share()) {}

  kv::Status Wait() {
    if (!used.exchange(true)) {
      entered_signal.set_value();
      if (release.wait_for(10s) != std::future_status::ready) {
        return kv::Status::IOError("test failed to release WAL sync gate");
      }
    }
    return kv::Status::OK();
  }

  std::promise<void> entered_signal;
  std::promise<void> release_signal;
  std::future<void> entered;
  std::shared_future<void> release;
  std::atomic<bool> used{false};
};

void TestTimeoutCanFollowDurableWrite() {
  WriteGate gate;
  kv::Options options;
  options.testing_before_wal_sync = [&] { return gate.Wait(); };
  Fixture fixture("timeout", options);
  kv::rpc::Client client(kEndpoint);
  auto pending = std::async(std::launch::async, [&] {
    return client.Put("timed-out", "eventually-persisted", 500ms);
  });
  CHECK(gate.entered.wait_for(3s) == std::future_status::ready);
  CHECK(pending.wait_for(3s) == std::future_status::ready);
  const auto result = pending.get();
  CHECK(!result.ok());
  CHECK(result.transport_status.code() == minirpc::StatusCode::kTimeout);
  CHECK(result.commit_state == CommitState::Unknown);
  gate.release_signal.set_value();

  fixture.server.Stop(5s);
  client.Close();
  MustOK(fixture.db.Close());
  MustOK(fixture.db.Open());
  std::string value;
  MustOK(fixture.db.Get("timed-out", &value));
  CHECK(value == "eventually-persisted");
  MustOK(fixture.db.Close());
}

void TestStopDrainsAcceptedWriteBeforeClosingDB() {
  WriteGate gate;
  kv::Options options;
  options.testing_before_wal_sync = [&] { return gate.Wait(); };
  Fixture fixture("drain", options);
  kv::rpc::Client client(kEndpoint);
  auto pending = std::async(std::launch::async, [&] {
    return client.Put("drained", "persisted", 5s);
  });
  CHECK(gate.entered.wait_for(3s) == std::future_status::ready);
  auto stopping = std::async(std::launch::async, [&] { fixture.server.Stop(5s); });
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (fixture.server.shutdown_state() == minirpc::ShutdownState::kRunning &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(fixture.server.shutdown_state() == minirpc::ShutdownState::kDraining);
  CHECK(stopping.wait_for(0ms) == std::future_status::timeout);
  gate.release_signal.set_value();
  CHECK(pending.wait_for(3s) == std::future_status::ready);
  const auto result = pending.get();
  MustOK(result);
  CHECK(result.commit_state == CommitState::Committed);
  CHECK(stopping.wait_for(3s) == std::future_status::ready);
  stopping.get();
  client.Close();
  MustOK(fixture.db.Close());
  MustOK(fixture.db.Open());
  std::string value;
  MustOK(fixture.db.Get("drained", &value));
  CHECK(value == "persisted");
  MustOK(fixture.db.Close());
}

void TestWALFailureReportsUnknownAndRecovers() {
  std::atomic<bool> fail{true};
  kv::Options options;
  options.testing_before_wal_sync = [&] {
    return fail.load() ? kv::Status::IOError("injected WAL sync failure")
                       : kv::Status::OK();
  };
  Fixture fixture("wal-error", options);
  kv::rpc::Client client(kEndpoint);
  kv::WriteBatch batch;
  batch.Put("recovered-a", "one");
  batch.Put("recovered-b", "two");
  const auto result = client.Write(batch);
  CHECK(!result.ok());
  CHECK(result.transport_status.ok());
  CHECK(result.status.IsIOError());
  CHECK(result.commit_state == CommitState::Unknown);
  const auto subsequent = client.Put("rejected-after-error", "absent");
  CHECK(!subsequent.ok());
  CHECK(subsequent.status.IsIOError());
  client.Close();
  fixture.server.Stop(5s);
  CHECK(fixture.db.Close().IsIOError());
  fail = false;
  MustOK(fixture.db.Open());
  MustOK(fixture.server.Start(kEndpoint));
  kv::rpc::Client reopened(kEndpoint);
  CheckValue(reopened, "recovered-a", "one");
  CheckValue(reopened, "recovered-b", "two");
  CheckMissing(reopened, "rejected-after-error");
  reopened.Close();
  fixture.server.Stop(5s);
  MustOK(fixture.db.Close());
}

}  // namespace

int main() {
  TestClientRejectsBeforeConnecting();
  TestBasicBatchAndReopen();
  TestInvalidRequestsAreAtomic();
  TestRequestLimits();
  TestConcurrentClients();
  TestTimeoutCanFollowDurableWrite();
  TestStopDrainsAcceptedWriteBeforeClosingDB();
  TestWALFailureReportsUnknownAndRecovers();
  std::cout << "RPC integration tests passed\n";
  return 0;
}
