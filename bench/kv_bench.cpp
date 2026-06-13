#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#include "db.h"

namespace {

struct Args {
  std::filesystem::path path = "bench_db";
  std::string mode = "write";
  size_t count = 100000;
  size_t value_size = 16;
  std::uint32_t seed = 1;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--path" && i + 1 < argc) {
      args->path = argv[++i];
    } else if ((arg == "--write" || arg == "--read" || arg == "--mixed" || arg == "--scan") &&
               i + 1 < argc) {
      args->mode = arg.substr(2);
      args->count = static_cast<size_t>(std::stoull(argv[++i]));
    } else if (arg == "--read-random" && i + 1 < argc) {
      args->mode = "read-random";
      args->count = static_cast<size_t>(std::stoull(argv[++i]));
    } else if (arg == "--value-size" && i + 1 < argc) {
      args->value_size = static_cast<size_t>(std::stoull(argv[++i]));
    } else if (arg == "--seed" && i + 1 < argc) {
      args->seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else {
      return false;
    }
  }
  return true;
}

std::uintmax_t DirectorySize(const std::filesystem::path& path) {
  std::uintmax_t size = 0;
  if (!std::filesystem::exists(path)) {
    return 0;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (entry.is_regular_file()) {
      size += entry.file_size();
    }
  }
  return size;
}

void PrintUsage() {
  std::cerr << "usage: kv_bench --path ./testdb --write|--read|--read-random|--mixed|--scan N "
               "[--value-size BYTES] [--seed N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage();
    return 1;
  }

  kv::DB db(args.path);
  kv::Status status = db.Open();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  size_t writes = 0;
  size_t reads = 0;
  size_t scan_items = 0;
  std::mt19937 rng(args.seed);
  std::uniform_int_distribution<size_t> dist(0, args.count == 0 ? 0 : args.count - 1);
  const std::string value_payload(args.value_size, 'x');
  const auto start = std::chrono::steady_clock::now();
  if (args.mode == "scan") {
    auto iterator = db.NewIterator();
    for (iterator->SeekToFirst(); iterator->Valid() && scan_items < args.count; iterator->Next()) {
      ++scan_items;
    }
    status = iterator->status();
  } else {
    for (size_t i = 0; i < args.count; ++i) {
      const std::string key = "bench_key_" + std::to_string(i);
      if (args.mode == "write") {
        status = db.Put(key, value_payload);
        ++writes;
      } else if (args.mode == "read") {
        std::string value;
        status = db.Get(key, &value);
        ++reads;
        if (status.IsNotFound()) {
          status = kv::Status::OK();
        }
      } else if (args.mode == "read-random") {
        std::string value;
        status = db.Get("bench_key_" + std::to_string(dist(rng)), &value);
        ++reads;
        if (status.IsNotFound()) {
          status = kv::Status::OK();
        }
      } else if (args.mode == "mixed") {
        if (i % 2 == 0) {
          status = db.Put(key, value_payload);
          ++writes;
        } else {
          std::string value;
          status = db.Get("bench_key_" + std::to_string(i / 2), &value);
          ++reads;
          if (status.IsNotFound()) {
            status = kv::Status::OK();
          }
        }
      } else {
        PrintUsage();
        return 1;
      }

      if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
      }
    }
  }
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }
  const auto end = std::chrono::steady_clock::now();

  const auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double elapsed_s = static_cast<double>(elapsed_us) / 1000000.0;
  const size_t operations = args.mode == "scan" ? scan_items : args.count;
  const double qps = elapsed_s == 0.0 ? 0.0 : static_cast<double>(operations) / elapsed_s;
  const double avg_us = operations == 0 ? 0.0 : static_cast<double>(elapsed_us) / operations;
  const kv::DBStats stats = db.Stats();

  std::cout << "writes: " << writes << '\n';
  std::cout << "reads: " << reads << '\n';
  std::cout << "scan_items: " << scan_items << '\n';
  std::cout << "elapsed_sec: " << elapsed_s << '\n';
  std::cout << "qps: " << qps << '\n';
  std::cout << "avg_latency_us: " << avg_us << '\n';
  std::cout << "sstables: " << stats.sstable_count << '\n';
  std::cout << "compactions: " << stats.compaction_count << '\n';
  std::cout << "cache_hits: " << stats.cache_hits << '\n';
  std::cout << "cache_misses: " << stats.cache_misses << '\n';
  std::cout << "bloom_filtered: " << stats.bloom_filtered << '\n';
  std::cout << "block_reads: " << stats.block_reads << '\n';
  std::cout << "db_size_bytes: " << DirectorySize(args.path) << '\n';
  return 0;
}
