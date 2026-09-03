#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db.h"

namespace {

struct Args {
  std::filesystem::path path = "week5_bench_db";
  std::string mode = "sync";
  std::size_t operations = 1000000;
  std::size_t threads = 4;
  std::size_t batch_size = 32;
  std::size_t value_size = 100;
};

bool ParsePositive(const std::string& text, std::size_t* value) {
  if (text.empty() ||
      std::any_of(text.begin(), text.end(), [](char ch) {
        return ch < '0' || ch > '9';
      })) {
    return false;
  }
  try {
    std::size_t parsed_chars = 0;
    const unsigned long long parsed = std::stoull(text, &parsed_chars);
    if (parsed_chars != text.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    *value = static_cast<std::size_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode" && i + 1 < argc) {
      args->mode = argv[++i];
      if (args->mode != "sync" && args->mode != "db-put" &&
          args->mode != "db-batch") {
        return false;
      }
    } else if (arg == "--path" && i + 1 < argc) {
      args->path = argv[++i];
    } else if (arg == "--operations" && i + 1 < argc) {
      if (!ParsePositive(argv[++i], &args->operations)) {
        return false;
      }
    } else if (arg == "--threads" && i + 1 < argc) {
      if (!ParsePositive(argv[++i], &args->threads)) {
        return false;
      }
    } else if (arg == "--batch-size" && i + 1 < argc) {
      if (!ParsePositive(argv[++i], &args->batch_size)) {
        return false;
      }
    } else if (arg == "--value-size" && i + 1 < argc) {
      if (!ParsePositive(argv[++i], &args->value_size)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

void PrintUsage() {
  std::cerr << "usage: week5_bench --mode sync|db-put|db-batch "
               "[--operations N] [--threads N] [--batch-size N] "
               "[--value-size BYTES] [--path PATH]\n";
}

struct CounterResult {
  double elapsed_seconds = 0.0;
  std::uint64_t counter = 0;
};

template <typename Worker>
CounterResult RunWorkers(std::size_t operations, std::size_t thread_count,
                         Worker worker) {
  const std::size_t actual_threads = std::min(operations, thread_count);
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(actual_threads);

  for (std::size_t thread_index = 0; thread_index < actual_threads;
       ++thread_index) {
    const std::size_t first = operations / actual_threads * thread_index +
                              std::min(thread_index, operations % actual_threads);
    const std::size_t count = operations / actual_threads +
                              (thread_index < operations % actual_threads ? 1 : 0);
    workers.emplace_back([&, thread_index, first, count] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      worker(thread_index, first, count);
    });
  }

  while (ready.load(std::memory_order_acquire) != actual_threads) {
    std::this_thread::yield();
  }
  const auto begin = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);
  for (auto& worker_thread : workers) {
    worker_thread.join();
  }
  const auto end = std::chrono::steady_clock::now();

  CounterResult result;
  result.elapsed_seconds =
      std::chrono::duration<double>(end - begin).count();
  return result;
}

void PrintSyncResult(const std::string& mode, const Args& args,
                     const CounterResult& result, bool lock_free) {
  const double ops_per_second =
      result.elapsed_seconds == 0.0
          ? 0.0
          : static_cast<double>(args.operations) / result.elapsed_seconds;
  const double ns_per_operation =
      static_cast<double>(result.elapsed_seconds * 1000000000.0) /
      static_cast<double>(args.operations);
  std::cout << mode << ',' << args.operations << ',' << args.threads << ','
            << args.batch_size << ',' << result.elapsed_seconds << ','
            << ops_per_second << ',' << ns_per_operation << ','
            << (lock_free ? "true" : "false") << ',' << result.counter << '\n';
}

int RunSyncBench(const Args& args) {
  std::mutex mutex;
  std::uint64_t mutex_counter = 0;
  CounterResult mutex_result = RunWorkers(
      args.operations, args.threads, [&](std::size_t, std::size_t, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
          std::lock_guard<std::mutex> lock(mutex);
          ++mutex_counter;
        }
      });
  mutex_result.counter = mutex_counter;
  PrintSyncResult("mutex", args, mutex_result, false);

  std::atomic<std::uint64_t> atomic_counter{0};
  CounterResult atomic_result = RunWorkers(
      args.operations, args.threads, [&](std::size_t, std::size_t, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
          atomic_counter.fetch_add(1, std::memory_order_relaxed);
        }
      });
  atomic_result.counter = atomic_counter.load(std::memory_order_relaxed);
  PrintSyncResult("atomic", args, atomic_result,
                  atomic_counter.is_lock_free());

  std::mutex batched_mutex;
  std::uint64_t batched_counter = 0;
  CounterResult batched_result = RunWorkers(
      args.operations, args.threads, [&](std::size_t, std::size_t, std::size_t count) {
        std::size_t pending = 0;
        for (std::size_t i = 0; i < count; ++i) {
          ++pending;
          if (pending == args.batch_size) {
            std::lock_guard<std::mutex> lock(batched_mutex);
            batched_counter += pending;
            pending = 0;
          }
        }
        if (pending != 0) {
          std::lock_guard<std::mutex> lock(batched_mutex);
          batched_counter += pending;
        }
      });
  batched_result.counter = batched_counter;
  PrintSyncResult("batched-mutex", args, batched_result, false);
  return (mutex_result.counter == args.operations &&
          atomic_result.counter == args.operations &&
          batched_result.counter == args.operations)
             ? 0
             : 1;
}

int RunDBBench(const Args& args) {
  kv::Options options;
  options.db_path = args.path;
  options.memtable_entries_limit =
      args.operations == std::numeric_limits<std::size_t>::max()
          ? args.operations
          : args.operations + 1;
  options.block_cache_capacity = 0;
  kv::DB db(options);
  kv::Status status = db.Open();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  const std::string value(args.value_size, 'x');
  const auto begin = std::chrono::steady_clock::now();
  std::size_t completed = 0;
  if (args.mode == "db-put") {
    for (std::size_t i = 0; i < args.operations; ++i) {
      status = db.Put("week5_key_" + std::to_string(i), value);
      if (!status.ok()) {
        break;
      }
      ++completed;
    }
  } else {
    for (std::size_t first = 0; first < args.operations;) {
      kv::WriteBatch batch;
      const std::size_t count =
          std::min(args.operations - first, args.batch_size);
      const std::size_t end = first + count;
      for (std::size_t i = first; i < end; ++i) {
        batch.Put("week5_key_" + std::to_string(i), value);
      }
      status = db.Write(batch);
      if (!status.ok()) {
        break;
      }
      completed += end - first;
      first = end;
    }
  }
  const auto end = std::chrono::steady_clock::now();
  const kv::Status close_status = db.Close();
  if (status.ok() && !close_status.ok()) {
    status = close_status;
  }
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();
  const double ops_per_second =
      elapsed_seconds == 0.0 ? 0.0 : completed / elapsed_seconds;
  const double ns_per_operation =
      completed == 0 ? 0.0 : elapsed_seconds * 1000000000.0 / completed;
  std::cout << args.mode << ',' << completed << ',' << 1 << ','
            << args.batch_size << ',' << elapsed_seconds << ','
            << ops_per_second << ',' << ns_per_operation << ",n/a,n/a\n";
  return completed == args.operations ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage();
    return 1;
  }

  std::cout << "mode,operations,threads,batch_size,elapsed_sec,ops_per_sec,ns_per_op,lock_free,counter\n";
  if (args.mode == "sync") {
    return RunSyncBench(args);
  }
  return RunDBBench(args);
}
