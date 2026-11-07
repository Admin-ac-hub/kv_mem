#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <vector>

#include "write_batch.h"

namespace kv::detail {

// The queue mutex protects admission and completion only. A single caller
// leads each FIFO prefix; storage/index locks are acquired by commit(), with
// this mutex released. Request/batch lifetimes extend through Submit().
template <class Result>
class WriteQueue {
 public:
  void Start() {
    std::lock_guard<std::mutex> lock(mu_);
    accepting_ = true;
  }

  void StopAndDrain() {
    std::unique_lock<std::mutex> lock(mu_);
    accepting_ = false;
    drained_.wait(lock, [&] { return queue_.empty() && !active_; });
  }

  bool Accepting() const {
    std::lock_guard<std::mutex> lock(mu_);
    return accepting_;
  }

  template <class Commit, class Error, class Budget>
  Result Submit(const WriteBatch& batch, bool grouping, size_t byte_limit,
                const std::function<void()>& after_enqueue, Commit commit,
                Error error, Budget budget) {
    Request request{&batch, batch.EncodedSize()};
    std::unique_lock<std::mutex> lock(mu_);
    if (!accepting_)
      return error(Status::IOError("database is not open or is closing"));
    queue_.push_back(&request);
    // Test notification only: must not block or reenter the database.
    try {
      if (after_enqueue) after_enqueue();
    } catch (const std::exception& exception) {
      queue_.pop_back();
      if (queue_.empty() && !active_) drained_.notify_all();
      return error(Status::IOError(exception.what()));
    } catch (...) {
      queue_.pop_back();
      if (queue_.empty() && !active_) drained_.notify_all();
      return error(Status::IOError("unexpected enqueue hook exception"));
    }
    request.cv.wait(lock, [&] {
      return request.done || (!active_ && queue_.front() == &request);
    });
    if (request.done) return request.result;
    active_ = true;
    // The common uncontended path needs neither a heap-allocated group nor a
    // second storage-state lock to determine a grouping budget. Choose this
    // request now; arrivals during its commit join the following FIFO group.
    const bool single = queue_.size() == 1 || !grouping || batch.Count() == 0 ||
                        request.bytes > byte_limit;
    if (single) {
      lock.unlock();
      Result result;
      try {
        result = commit(batch);
      } catch (const std::exception& exception) {
        result = error(Status::IOError(exception.what()));
      } catch (...) {
        result = error(Status::IOError("unexpected write group exception"));
      }
      lock.lock();
      FinishLocked(1, result);
      return request.result;
    }
    lock.unlock();

    std::vector<Request*> group;
    Result result;
    try {
      const auto operation_limit = std::min<std::uint64_t>(
          budget(), std::numeric_limits<std::uint32_t>::max());
      lock.lock();
      size_t bytes = request.bytes;
      size_t operations = batch.Count();
      group.push_back(&request);
      const size_t limit = std::min<size_t>(
          byte_limit, std::numeric_limits<std::uint32_t>::max());
      if (bytes <= limit && operations <= operation_limit) {
        for (size_t i = 1; i < queue_.size(); ++i) {
          Request* next = queue_[i];
          // Empty writes get their own turn, including health checks.
          if (next->batch->Count() == 0 || next->bytes > limit - bytes ||
              next->batch->Count() > operation_limit - operations)
            break;
          group.push_back(next);
          bytes += next->bytes;
          operations += next->batch->Count();
        }
      }
      lock.unlock();
      if (group.size() == 1) {
        result = commit(batch);
      } else {
        WriteBatch combined;
        combined.Reserve(operations);
        for (auto* writer : group) combined.Append(*writer->batch);
        result = commit(combined);
      }
    } catch (const std::exception& exception) {
      result = error(Status::IOError(exception.what()));
    } catch (...) {
      result = error(Status::IOError("unexpected write group exception"));
    }
    if (!lock.owns_lock()) lock.lock();
    // Selection/allocation may have failed before even recording the leader.
    const size_t count = group.empty() ? 1 : group.size();
    FinishLocked(count, result);
    return request.result;
  }

 private:
  void FinishLocked(size_t count, const Result& result) {
    for (size_t i = 0; i < count; ++i) {
      Request* writer = queue_.front();
      queue_.pop_front();
      writer->result = result;
      writer->done = true;
      writer->cv.notify_one();
    }
    active_ = false;
    if (!queue_.empty())
      queue_.front()->cv.notify_one();
    else
      drained_.notify_all();
  }
  struct Request {
    Request(const WriteBatch* input, size_t encoded_bytes)
        : batch(input), bytes(encoded_bytes) {}
    const WriteBatch* batch;
    size_t bytes;
    Result result{};
    bool done = false;
    std::condition_variable cv;
  };
  mutable std::mutex mu_;
  std::condition_variable drained_;
  std::deque<Request*> queue_;
  bool accepting_ = false;
  bool active_ = false;
};

}  // namespace kv::detail
