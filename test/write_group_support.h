#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "test_support.h"

// Block the first two WAL syncs. Followers enqueue while the leader is syncing,
// then form a deterministic second FIFO group.
class WriteGate {
 public:
  void Enqueued() {
    std::lock_guard<std::mutex> lock(mu_);
    ++enqueued_;
    cv_.notify_all();
  }
  kv::Status Sync() {
    std::unique_lock<std::mutex> lock(mu_);
    const int call = ++syncs_;
    cv_.notify_all();
    if (call <= 2) cv_.wait(lock, [&] { return released_ >= call; });
    return fail_second && call == 2
               ? kv::Status::IOError("injected group sync error")
               : kv::Status::OK();
  }
  void WaitEnqueued(int count) {
    Wait([&] { return enqueued_ >= count; });
  }
  void WaitSync(int count) {
    Wait([&] { return syncs_ >= count; });
  }
  void Release(int count) {
    std::lock_guard<std::mutex> lock(mu_);
    released_ = count;
    cv_.notify_all();
  }
  bool fail_second = false;

 private:
  template <class Predicate>
  void Wait(Predicate predicate) {
    std::unique_lock<std::mutex> lock(mu_);
    CHECK(cv_.wait_for(lock, std::chrono::seconds(10), predicate));
  }
  std::mutex mu_;
  std::condition_variable cv_;
  int enqueued_ = 0, syncs_ = 0, released_ = 0;
};
