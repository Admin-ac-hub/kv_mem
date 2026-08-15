#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "db.h"
#include "write_batch.h"

namespace {

struct Args {
  std::filesystem::path path = "stress_runs/stress_db";
  size_t epochs = 10;
  size_t threads = 4;
  size_t ops_per_thread = 2000;
  size_t keys_per_thread = 1000;
  size_t value_size = 100;
  int delete_percent = 20;
  size_t batch_size = 1;
  size_t reopen_every_epochs = 2;
  size_t compact_every_epochs = 3;
  std::uint32_t seed = 1;
  size_t memtable_limit = 128;
  size_t l0_limit = 2;
};

enum class OpType {
  kPut,
  kDelete,
};

struct AppliedOp {
  std::uint64_t op_id = 0;
  OpType type = OpType::kPut;
  std::string key;
  std::string value;
};

struct Counters {
  std::uint64_t puts = 0;
  std::uint64_t deletes = 0;
  std::uint64_t reads = 0;
  std::uint64_t batches = 0;
};

void PrintUsage() {
  std::cerr
      << "usage: kv_stress [--path DIR] [--epochs N] [--threads N] "
      << "[--ops-per-thread N] [--keys-per-thread N] [--value-size N] "
      << "[--delete-percent N] [--batch-size N] [--reopen-every-epochs N] "
      << "[--compact-every-epochs N] [--seed N] [--memtable-limit N] "
      << "[--l0-limit N]\n";
}

bool ParseSize(const std::string& text, size_t* value) {
  try {
    size_t parsed = 0;
    const unsigned long long number = std::stoull(text, &parsed);
    if (parsed != text.size()) {
      return false;
    }
    *value = static_cast<size_t>(number);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseInt(const std::string& text, int* value) {
  try {
    size_t parsed = 0;
    const int number = std::stoi(text, &parsed);
    if (parsed != text.size()) {
      return false;
    }
    *value = number;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << '\n';
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--path") {
      const char* value = require_value(arg);
      if (value == nullptr) return false;
      args->path = value;
    } else if (arg == "--epochs") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->epochs)) return false;
    } else if (arg == "--threads") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->threads)) return false;
    } else if (arg == "--ops-per-thread") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->ops_per_thread)) return false;
    } else if (arg == "--keys-per-thread") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->keys_per_thread)) return false;
    } else if (arg == "--value-size") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->value_size)) return false;
    } else if (arg == "--delete-percent") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseInt(value, &args->delete_percent)) return false;
    } else if (arg == "--batch-size") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->batch_size)) return false;
    } else if (arg == "--reopen-every-epochs") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->reopen_every_epochs)) return false;
    } else if (arg == "--compact-every-epochs") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->compact_every_epochs)) return false;
    } else if (arg == "--seed") {
      size_t seed = 0;
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &seed)) return false;
      args->seed = static_cast<std::uint32_t>(seed);
    } else if (arg == "--memtable-limit") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->memtable_limit)) return false;
    } else if (arg == "--l0-limit") {
      const char* value = require_value(arg);
      if (value == nullptr || !ParseSize(value, &args->l0_limit)) return false;
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return false;
    }
  }

  if (args->threads == 0 || args->ops_per_thread == 0 || args->keys_per_thread == 0 ||
      args->batch_size == 0 || args->delete_percent < 0 || args->delete_percent > 100) {
    return false;
  }
  return true;
}

std::string KeyFor(size_t thread_id, size_t slot) {
  return "stress_t" + std::to_string(thread_id) + "_k" + std::to_string(slot);
}

std::string MakeValue(size_t epoch, size_t thread_id, size_t slot,
                      std::uint64_t op_id, size_t value_size) {
  std::ostringstream out;
  out << "epoch=" << epoch << ";tid=" << thread_id << ";slot=" << slot
      << ";op=" << op_id << ';';
  std::string value = out.str();
  if (value.size() < value_size) {
    value.append(value_size - value.size(), 'x');
  }
  return value;
}

kv::Options MakeOptions(const Args& args) {
  kv::Options options;
  options.db_path = args.path;
  options.memtable_entries_limit = args.memtable_limit;
  options.level0_sstable_limit = args.l0_limit;
  return options;
}

bool OpenDB(const Args& args, std::unique_ptr<kv::DB>* db) {
  auto next = std::make_unique<kv::DB>(MakeOptions(args));
  kv::Status status = next->Open();
  if (!status.ok()) {
    std::cerr << "open failed: " << status.ToString() << '\n';
    return false;
  }
  *db = std::move(next);
  return true;
}

void ApplyOpsToModel(const std::vector<AppliedOp>& ops,
                     std::map<std::string, std::string>* model) {
  std::vector<AppliedOp> sorted = ops;
  std::sort(sorted.begin(), sorted.end(), [](const AppliedOp& lhs, const AppliedOp& rhs) {
    return lhs.op_id < rhs.op_id;
  });
  for (const auto& op : sorted) {
    if (op.type == OpType::kPut) {
      (*model)[op.key] = op.value;
    } else {
      model->erase(op.key);
    }
  }
}

bool VerifyIterator(kv::DB* db, const std::map<std::string, std::string>& model,
                    const std::string& label) {
  std::map<std::string, std::string> actual;
  auto iterator = db->NewIterator();
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    actual[iterator->key()] = iterator->value();
  }
  if (!iterator->status().ok()) {
    std::cerr << label << ": iterator failed: " << iterator->status().ToString() << '\n';
    return false;
  }
  auto actual_it = actual.begin();
  auto expected_it = model.begin();
  for (; actual_it != actual.end() && expected_it != model.end(); ++actual_it, ++expected_it) {
    if (actual_it->first != expected_it->first) {
      std::cerr << label << ": key mismatch actual=" << actual_it->first
                << " expected=" << expected_it->first << '\n';
      return false;
    }
    if (actual_it->second != expected_it->second) {
      std::cerr << label << ": value mismatch key=" << actual_it->first
                << " actual=" << actual_it->second
                << " expected=" << expected_it->second << '\n';
      return false;
    }
  }
  if (actual_it != actual.end()) {
    std::cerr << label << ": unexpected extra key=" << actual_it->first
              << " value=" << actual_it->second << '\n';
    return false;
  }
  if (expected_it != model.end()) {
    std::cerr << label << ": missing key=" << expected_it->first
              << " expected=" << expected_it->second << '\n';
    return false;
  }
  return true;
}

bool VerifyPointReads(kv::DB* db, const std::map<std::string, std::string>& model,
                      const Args& args, const std::string& label) {
  size_t checked = 0;
  for (const auto& [key, expected] : model) {
    if (checked >= 64) {
      break;
    }
    std::string actual;
    kv::Status status = db->Get(key, &actual);
    if (!status.ok()) {
      std::cerr << label << ": expected key missing: " << key << ": "
                << status.ToString() << '\n';
      return false;
    }
    if (actual != expected) {
      std::cerr << label << ": point read mismatch key=" << key
                << " actual=" << actual << " expected=" << expected << '\n';
      return false;
    }
    ++checked;
  }

  for (size_t thread = 0; thread < args.threads && checked < 128; ++thread) {
    for (size_t slot = args.keys_per_thread; slot < args.keys_per_thread + 16 && checked < 128; ++slot) {
      const std::string key = KeyFor(thread, slot);
      std::string actual;
      kv::Status status = db->Get(key, &actual);
      if (!status.IsNotFound()) {
        std::cerr << label << ": expected absent key status NotFound for " << key
                  << ", got " << status.ToString() << '\n';
        return false;
      }
      ++checked;
    }
  }
  return true;
}

bool VerifyDB(kv::DB* db, const std::map<std::string, std::string>& model,
              const Args& args, const std::string& label) {
  return VerifyIterator(db, model, label) && VerifyPointReads(db, model, args, label);
}

void PrintStats(kv::DB* db, size_t epoch, const Counters& counters, size_t model_size) {
  const kv::DBStats stats = db->Stats();
  std::cout << "epoch=" << epoch
            << " puts=" << counters.puts
            << " deletes=" << counters.deletes
            << " reads=" << counters.reads
            << " batches=" << counters.batches
            << " model_size=" << model_size
            << " sstables=" << stats.sstable_count
            << " compactions=" << stats.compaction_count
            << " cache_hits=" << stats.cache_hits
            << " cache_misses=" << stats.cache_misses
            << " bloom_filtered=" << stats.bloom_filtered
            << " block_reads=" << stats.block_reads
            << '\n';
}

bool RunEpoch(kv::DB* db, const Args& args, size_t epoch,
              std::atomic<std::uint64_t>* next_op_id,
              std::vector<AppliedOp>* epoch_ops, Counters* counters) {
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  std::vector<std::vector<AppliedOp>> logs(args.threads);
  std::vector<Counters> local_counters(args.threads);

  for (size_t thread_id = 0; thread_id < args.threads; ++thread_id) {
    workers.emplace_back([&, thread_id] {
      std::mt19937 rng(args.seed + static_cast<std::uint32_t>(epoch * 9973 + thread_id * 131));
      std::uniform_int_distribution<size_t> key_dist(0, args.keys_per_thread - 1);
      std::uniform_int_distribution<int> op_dist(0, 99);
      size_t op_index = 0;
      while (op_index < args.ops_per_thread && !failed.load()) {
        const size_t remaining = args.ops_per_thread - op_index;
        const size_t batch_ops = std::min(args.batch_size, remaining);
        kv::WriteBatch batch;
        std::vector<AppliedOp> pending;
        pending.reserve(batch_ops);

        for (size_t batch_index = 0; batch_index < batch_ops; ++batch_index) {
          const size_t slot = key_dist(rng);
          const std::string key = KeyFor(thread_id, slot);
          const std::uint64_t op_id = next_op_id->fetch_add(1);
          if (op_dist(rng) < args.delete_percent) {
            batch.Delete(key);
            pending.push_back(AppliedOp{op_id, OpType::kDelete, key, ""});
          } else {
            std::string value = MakeValue(epoch, thread_id, slot, op_id, args.value_size);
            batch.Put(key, value);
            pending.push_back(AppliedOp{op_id, OpType::kPut, key, value});
          }
        }

        kv::Status status = batch.Count() == 1 && pending.front().type == OpType::kPut
                                ? db->Put(pending.front().key, pending.front().value)
                                : batch.Count() == 1 && pending.front().type == OpType::kDelete
                                      ? db->Delete(pending.front().key)
                                      : db->Write(batch);
        if (!status.ok()) {
          std::cerr << "worker " << thread_id << " write failed: " << status.ToString() << '\n';
          failed = true;
          return;
        }

        for (const auto& op : pending) {
          if (op.type == OpType::kPut) {
            ++local_counters[thread_id].puts;
          } else {
            ++local_counters[thread_id].deletes;
          }
          logs[thread_id].push_back(op);
        }
        ++local_counters[thread_id].batches;
        op_index += batch_ops;

        if (op_index % 17 == 0) {
          std::string value;
          kv::Status read_status = db->Get(KeyFor(thread_id, key_dist(rng)), &value);
          if (!read_status.ok() && !read_status.IsNotFound()) {
            std::cerr << "worker " << thread_id << " read failed: "
                      << read_status.ToString() << '\n';
            failed = true;
            return;
          }
          ++local_counters[thread_id].reads;
        }
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }
  if (failed.load()) {
    return false;
  }

  for (size_t thread_id = 0; thread_id < args.threads; ++thread_id) {
    epoch_ops->insert(epoch_ops->end(), logs[thread_id].begin(), logs[thread_id].end());
    counters->puts += local_counters[thread_id].puts;
    counters->deletes += local_counters[thread_id].deletes;
    counters->reads += local_counters[thread_id].reads;
    counters->batches += local_counters[thread_id].batches;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage();
    return 1;
  }

  std::filesystem::create_directories(args.path.parent_path());
  std::filesystem::remove_all(args.path);

  std::unique_ptr<kv::DB> db;
  if (!OpenDB(args, &db)) {
    return 1;
  }

  std::map<std::string, std::string> model;
  std::atomic<std::uint64_t> next_op_id{1};
  Counters counters;

  std::cout << "kv_stress start path=" << args.path
            << " epochs=" << args.epochs
            << " threads=" << args.threads
            << " ops_per_thread=" << args.ops_per_thread
            << " keys_per_thread=" << args.keys_per_thread
            << " seed=" << args.seed << '\n';

  for (size_t epoch = 1; epoch <= args.epochs; ++epoch) {
    std::vector<AppliedOp> epoch_ops;
    if (!RunEpoch(db.get(), args, epoch, &next_op_id, &epoch_ops, &counters)) {
      return 1;
    }
    ApplyOpsToModel(epoch_ops, &model);

    if (args.compact_every_epochs != 0 && epoch % args.compact_every_epochs == 0) {
      kv::Status status = db->Compact();
      if (!status.ok()) {
        std::cerr << "compact failed after epoch " << epoch << ": "
                  << status.ToString() << '\n';
        return 1;
      }
    }

    if (!VerifyDB(db.get(), model, args, "epoch " + std::to_string(epoch))) {
      return 1;
    }
    PrintStats(db.get(), epoch, counters, model.size());

    if (args.reopen_every_epochs != 0 && epoch % args.reopen_every_epochs == 0) {
      kv::Status status = db->Close();
      if (!status.ok()) {
        std::cerr << "close failed after epoch " << epoch << ": "
                  << status.ToString() << '\n';
        return 1;
      }
      db.reset();
      if (!OpenDB(args, &db)) {
        return 1;
      }
      if (!VerifyDB(db.get(), model, args, "reopen epoch " + std::to_string(epoch))) {
        return 1;
      }
      std::cout << "reopen verified epoch=" << epoch << '\n';
    }
  }

  kv::Status status = db->Close();
  if (!status.ok()) {
    std::cerr << "final close failed: " << status.ToString() << '\n';
    return 1;
  }
  db.reset();
  if (!OpenDB(args, &db)) {
    return 1;
  }
  if (!VerifyDB(db.get(), model, args, "final reopen")) {
    return 1;
  }
  PrintStats(db.get(), args.epochs, counters, model.size());
  std::cout << "kv_stress passed\n";
  return 0;
}
