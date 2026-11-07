#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db.h"

namespace {
using Clock = std::chrono::steady_clock;
struct Args {
  std::filesystem::path path;
  size_t operations = 100000;
  size_t threads = 4;
  size_t batch = 1;
  size_t value_size = 100;
  size_t memtable_limit = 1024;
  size_t l0_limit = 4;
  bool grouping = true;
};

bool Positive(const std::string& text, size_t* out) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    return false;
  try {
    const auto value = std::stoull(text);
    if (value == 0 || value > std::numeric_limits<size_t>::max()) return false;
    *out = static_cast<size_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool Parse(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (i + 1 == argc) return false;
    const std::string value = argv[++i];
    if (flag == "--path")
      args->path = value;
    else if (flag == "--grouping") {
      if (value != "on" && value != "off") return false;
      args->grouping = value == "on";
    } else {
      size_t* field = flag == "--operations"       ? &args->operations
                      : flag == "--threads"        ? &args->threads
                      : flag == "--batch-size"     ? &args->batch
                      : flag == "--value-size"     ? &args->value_size
                      : flag == "--memtable-limit" ? &args->memtable_limit
                      : flag == "--l0-limit"       ? &args->l0_limit
                                                   : nullptr;
      if (field == nullptr || !Positive(value, field)) return false;
    }
  }
#ifdef KV_GROUP_COMMIT_BASELINE
  if (args->grouping) return false;
#endif
  return !args->path.empty() && (!std::filesystem::exists(args->path) ||
                                 std::filesystem::is_empty(args->path));
}

std::string Key(size_t i) { return "bench:" + std::to_string(i); }

double Percentile(const std::vector<double>& sorted, double fraction) {
  return sorted.empty()
             ? 0
             : sorted[static_cast<size_t>(fraction * (sorted.size() - 1))];
}
}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!Parse(argc, argv, &args)) {
    std::cerr << "usage: group_commit_bench --path FRESH_DIR [--operations N] "
                 "[--threads N] "
                 "[--batch-size N] [--value-size N] "
                 "[--grouping on|off] "
                 "[--memtable-limit N] [--l0-limit N]\n";
    return 1;
  }
  std::atomic<std::uint64_t> observed_syncs{0};
  kv::Options options;
  options.db_path = args.path;
  options.memtable_entries_limit = args.memtable_limit;
  options.level0_sstable_limit = args.l0_limit;
  options.testing_before_wal_sync = [&] {
    observed_syncs.fetch_add(1, std::memory_order_relaxed);
    return kv::Status::OK();
  };
#ifndef KV_GROUP_COMMIT_BASELINE
  options.enable_write_grouping = args.grouping;
#endif
  kv::DB db(options);
  auto status = db.Open();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }
  const size_t threads = std::min(args.threads, args.operations);
  std::atomic<size_t> ready{0};
  std::atomic<bool> start{false};
  std::atomic<size_t> completed{0};
  std::vector<std::vector<double>> latencies(threads);
  std::vector<std::thread> workers;
  std::mutex error_mu;
  const std::string value(args.value_size, 'x');
  for (size_t t = 0; t < threads; ++t) {
    const size_t first =
        args.operations / threads * t + std::min(t, args.operations % threads);
    const size_t count =
        args.operations / threads + (t < args.operations % threads);
    workers.emplace_back([&, t, first, count] {
      latencies[t].reserve(count / args.batch + 1);
      ready.fetch_add(1);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t offset = 0; offset < count;) {
        const size_t n = std::min(args.batch, count - offset);
        kv::WriteBatch batch;
        if (n != 1)
          for (size_t j = 0; j < n; ++j)
            batch.Put(Key(first + offset + j), value);
        const auto begin = Clock::now();
        const auto s =
            n == 1 ? db.Put(Key(first + offset), value) : db.Write(batch);
        latencies[t].push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - begin)
                .count());
        if (!s.ok()) {
          std::lock_guard<std::mutex> lock(error_mu);
          status = s;
          return;
        }
        completed.fetch_add(n, std::memory_order_relaxed);
        offset += n;
      }
    });
  }
  while (ready.load() != threads) std::this_thread::yield();
  const auto begin = Clock::now();
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();
  const auto writes_done = Clock::now();
  const auto close_status = db.Close();
  const auto drained = Clock::now();
  if (!status.ok() || !close_status.ok() || completed != args.operations) {
    std::cerr << status.ToString() << "; " << close_status.ToString() << '\n';
    return 1;
  }
  const auto stats = db.Stats();
  std::vector<double> samples;
  for (const auto& thread : latencies)
    samples.insert(samples.end(), thread.begin(), thread.end());
  std::sort(samples.begin(), samples.end());
  // Reopen the authoritative KV store and verify every value outside timing.
  kv::Options verification_options;
  verification_options.db_path = args.path;
  kv::DB verification(verification_options);
  status = verification.Open();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }
  auto iterator = verification.NewIterator();
  size_t verified = 0;
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    const auto& key = iterator->key();
    if (key.rfind("bench:", 0) != 0) return 1;
    const size_t id = std::stoull(key.substr(6));
    if (id >= args.operations) return 1;
    if (iterator->value() != value) return 1;
    ++verified;
  }
  if (!iterator->status().ok() || verified != args.operations ||
      !verification.Close().ok())
    return 1;
  const double write_seconds =
      std::chrono::duration<double>(writes_done - begin).count();
  const double wall_seconds =
      std::chrono::duration<double>(drained - begin).count();
#ifdef KV_GROUP_COMMIT_BASELINE
  const auto syncs = observed_syncs.load();
  const auto committed = args.operations;
  const auto max_batch = std::min(args.batch, args.operations);
  const char* implementation = "baseline";
#else
  const auto syncs = stats.wal_sync_count;
  const auto committed = stats.wal_committed_operations;
  const auto max_batch = stats.max_wal_batch_operations;
  const char* implementation = args.grouping ? "on" : "off";
#endif
  const char* engine = "kv";
  std::cout
      << std::setprecision(12)
      << "engine,implementation,operations,threads,batch_size,write_"
         "seconds,wall_seconds,ops_per_sec,wall_ops_per_sec,request_p50_us,"
         "request_p99_us,wal_sync_count,operations_per_sync,max_wal_batch_"
         "operations,flush_count,compaction_count,verified\n"
      << engine << ',' << implementation << ',' << args.operations << ','
      << threads << ',' << args.batch << ','
      << write_seconds << ',' << wall_seconds << ','
      << args.operations / write_seconds << ','
      << args.operations / wall_seconds << ',' << Percentile(samples, 0.50)
      << ',' << Percentile(samples, 0.99) << ',' << syncs << ','
      << (syncs == 0 ? 0.0 : static_cast<double>(committed) / syncs) << ','
      << max_batch << ',' << stats.flush_count << ',' << stats.compaction_count
      << ',' << verified << '\n';
}
