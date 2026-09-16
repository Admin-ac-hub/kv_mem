#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db.h"

namespace {

struct Args {
  std::filesystem::path path = "vector_bench_db";
  std::string mode = "all";
  size_t vector_count = 1000000;
  size_t dimension = 768;
  size_t query_count = 1000;
  size_t recall_query_count = 10;
  size_t mixed_operations = 1000000;
  size_t write_batch_size = 256;
  size_t query_batch_size = 32;
  size_t top_k = 10;
  size_t ef_search = 128;
  size_t memtable_limit = 1024;
  size_t l0_limit = 4;
  size_t l1_bytes = 64ULL * 1024 * 1024;
  size_t block_cache_capacity = 64;
  size_t hnsw_m = 16;
  size_t ef_construction = 200;
  std::uint64_t seed = 20260907;
  bool reset = false;
  bool require_maintenance = false;
};

struct Timing {
  size_t operations = 0;
  double elapsed_seconds = 0.0;
  std::vector<double> per_operation_microseconds;
};

struct StorageUsage {
  std::uintmax_t sstable_bytes = 0;
  std::uintmax_t index_bytes = 0;
  std::uintmax_t wal_bytes = 0;
};

bool ParseSize(const char* text, size_t* value) {
  try {
    const std::string input(text);
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    const unsigned long long parsed = std::stoull(input);
    if (parsed > std::numeric_limits<size_t>::max()) {
      return false;
    }
    *value = static_cast<size_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseUint64(const char* text, std::uint64_t* value) {
  try {
    const std::string input(text);
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    *value = std::stoull(input);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto has_value = [&] { return i + 1 < argc; };
    if (arg == "--path" && has_value()) {
      args->path = argv[++i];
    } else if (arg == "--mode" && has_value()) {
      args->mode = argv[++i];
    } else if (arg == "--vectors" && has_value()) {
      if (!ParseSize(argv[++i], &args->vector_count)) {
        return false;
      }
    } else if (arg == "--dimension" && has_value()) {
      if (!ParseSize(argv[++i], &args->dimension)) {
        return false;
      }
    } else if (arg == "--queries" && has_value()) {
      if (!ParseSize(argv[++i], &args->query_count)) {
        return false;
      }
    } else if (arg == "--recall-queries" && has_value()) {
      if (!ParseSize(argv[++i], &args->recall_query_count)) {
        return false;
      }
    } else if (arg == "--mixed-operations" && has_value()) {
      if (!ParseSize(argv[++i], &args->mixed_operations)) {
        return false;
      }
    } else if (arg == "--write-batch" && has_value()) {
      if (!ParseSize(argv[++i], &args->write_batch_size)) {
        return false;
      }
    } else if (arg == "--query-batch" && has_value()) {
      if (!ParseSize(argv[++i], &args->query_batch_size)) {
        return false;
      }
    } else if (arg == "--top-k" && has_value()) {
      if (!ParseSize(argv[++i], &args->top_k)) {
        return false;
      }
    } else if (arg == "--ef-search" && has_value()) {
      if (!ParseSize(argv[++i], &args->ef_search)) {
        return false;
      }
    } else if (arg == "--seed" && has_value()) {
      if (!ParseUint64(argv[++i], &args->seed)) {
        return false;
      }
    } else if (arg == "--memtable-limit" && has_value()) {
      if (!ParseSize(argv[++i], &args->memtable_limit)) return false;
    } else if (arg == "--l0-limit" && has_value()) {
      if (!ParseSize(argv[++i], &args->l0_limit)) return false;
    } else if (arg == "--l1-bytes" && has_value()) {
      if (!ParseSize(argv[++i], &args->l1_bytes)) return false;
    } else if (arg == "--block-cache" && has_value()) {
      if (!ParseSize(argv[++i], &args->block_cache_capacity)) return false;
    } else if (arg == "--hnsw-m" && has_value()) {
      if (!ParseSize(argv[++i], &args->hnsw_m)) return false;
    } else if (arg == "--ef-construction" && has_value()) {
      if (!ParseSize(argv[++i], &args->ef_construction)) return false;
    } else if (arg == "--require-maintenance") {
      args->require_maintenance = true;
    } else if (arg == "--reset") {
      args->reset = true;
    } else {
      return false;
    }
  }
  return args->vector_count > 0 && args->dimension > 0 &&
         args->write_batch_size > 0 && args->query_batch_size > 0 &&
         args->top_k > 0 && args->ef_search > 0 &&
         args->memtable_limit > 0 && args->hnsw_m >= 2 &&
         args->ef_construction > 0 &&
         (args->mode == "write" || args->mode == "query" ||
          args->mode == "mixed" || args->mode == "all");
}

void PrintUsage() {
  std::cerr
      << "usage: vector_bench [--mode write|query|mixed|all] [--path PATH] "
         "[--vectors N] [--dimension N] [--queries N] "
         "[--recall-queries N] [--mixed-operations N] "
         "[--write-batch N] [--query-batch N] [--top-k N] "
         "[--ef-search N] [--seed N] [--reset] [--memtable-limit N] "
         "[--l0-limit N] [--l1-bytes N] [--block-cache N] "
         "[--hnsw-m N] [--ef-construction N] [--require-maintenance]\n";
}

std::uint64_t Mix(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

float RandomCoordinate(std::uint64_t seed, size_t vector_id, size_t dimension) {
  const std::uint64_t bits = Mix(seed ^ (static_cast<std::uint64_t>(vector_id) << 32U) ^
                                 static_cast<std::uint64_t>(dimension));
  constexpr double kScale = 1.0 / static_cast<double>(1U << 24U);
  return static_cast<float>(static_cast<double>(bits >> 40U) * kScale * 2.0 -
                            1.0);
}

std::vector<float> MakeVector(const Args& args, size_t vector_id) {
  std::vector<float> vector(args.dimension);
  for (size_t dimension = 0; dimension < args.dimension; ++dimension) {
    vector[dimension] = RandomCoordinate(args.seed, vector_id, dimension);
  }
  return vector;
}

std::vector<float> MakeQuery(const Args& args, size_t vector_id) {
  std::vector<float> query = MakeVector(args, vector_id);
  for (size_t dimension = 0; dimension < query.size(); ++dimension) {
    const float noise = RandomCoordinate(args.seed ^ 0xa5a5a5a5ULL,
                                         vector_id, dimension) * 0.001f;
    query[dimension] += noise;
  }
  return query;
}

std::string KeyFor(size_t vector_id) {
  return "vector:" + std::to_string(vector_id);
}

kv::Options MakeOptions(const Args& args) {
  kv::Options options;
  options.db_path = args.path;
  options.vector_dimension = args.dimension;
  options.hnsw_max_neighbors = args.hnsw_m;
  options.hnsw_ef_construction = args.ef_construction;
  options.memtable_entries_limit = args.memtable_limit;
  options.level0_sstable_limit = args.l0_limit;
  options.level1_size_limit_bytes = args.l1_bytes;
  options.block_cache_capacity = args.block_cache_capacity;
  return options;
}

StorageUsage GetStorageUsage(const std::filesystem::path& path) {
  StorageUsage usage;
  if (!std::filesystem::exists(path)) {
    return usage;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::uintmax_t size = entry.file_size();
    if (entry.path().extension() == ".data") {
      usage.sstable_bytes += size;
    } else if (entry.path().extension() == ".hnsw") {
      usage.index_bytes += size;
    } else if (entry.path().extension() == ".log") {
      usage.wal_bytes += size;
    }
  }
  return usage;
}

bool Open(kv::DB* db) {
  const kv::Status status = db->Open();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return false;
  }
  return true;
}

bool PrintMaintenance(const Args& args, const kv::DB& db) {
  const auto stats = db.Stats();
  std::cout << "flush_count=" << stats.flush_count << '\n';
  std::cout << "compaction_count=" << stats.compaction_count << '\n';
  std::cout << "sstable_count=" << stats.sstable_count << '\n';
  if (args.require_maintenance &&
      (stats.flush_count < 2 || stats.compaction_count == 0)) {
    std::cerr << "required flush/compaction path was not exercised\n";
    return false;
  }
  return true;
}

bool Ingest(kv::DB* db,
            const Args& args,
            size_t first_id,
            size_t count,
            Timing* timing) {
  const auto benchmark_start = std::chrono::steady_clock::now();
  for (size_t first = 0; first < count;) {
    const size_t batch_count = std::min(args.write_batch_size, count - first);
    kv::WriteBatch batch;
    for (size_t offset = 0; offset < batch_count; ++offset) {
      const size_t id = first_id + first + offset;
      const kv::Status status = batch.PutVector(KeyFor(id), MakeVector(args, id));
      if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return false;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    const kv::Status status = db->Write(batch);
    const auto end = std::chrono::steady_clock::now();
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return false;
    }
    timing->operations += batch_count;
    timing->per_operation_microseconds.push_back(
        std::chrono::duration<double, std::micro>(end - start).count() /
        static_cast<double>(batch_count));
    first += batch_count;
  }
  timing->elapsed_seconds +=
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    benchmark_start)
          .count();
  return true;
}

bool PopulateDatabase(const Args& args, size_t vector_count) {
  kv::DB db(MakeOptions(args));
  if (!Open(&db)) {
    return false;
  }
  Timing ignored;
  if (!Ingest(&db, args, 0, vector_count, &ignored)) {
    return false;
  }
  const kv::Status status = db.Close();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return false;
  }
  return PrintMaintenance(args, db);
}

double Percentile(std::vector<double> samples, double percentile) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double position = percentile * static_cast<double>(samples.size() - 1);
  return samples[static_cast<size_t>(std::ceil(position))];
}

void PrintTiming(const std::string& prefix, const Timing& timing) {
  const double throughput = timing.elapsed_seconds == 0.0
                                ? 0.0
                                : static_cast<double>(timing.operations) /
                                      timing.elapsed_seconds;
  std::cout << prefix << "_operations=" << timing.operations << '\n';
  std::cout << prefix << "_elapsed_seconds=" << timing.elapsed_seconds << '\n';
  std::cout << prefix << "_throughput_per_second=" << throughput << '\n';
  std::cout << prefix << "_batch_amortized_p50_us="
            << Percentile(timing.per_operation_microseconds, 0.50) << '\n';
  std::cout << prefix << "_batch_amortized_p99_us="
            << Percentile(timing.per_operation_microseconds, 0.99) << '\n';
}

bool TimedQueries(kv::DB* db,
                  const Args& args,
                  size_t corpus_count,
                  Timing* timing,
                  std::vector<size_t>* query_ids,
                  std::vector<std::vector<kv::VectorResult>>* query_results) {
  const auto benchmark_start = std::chrono::steady_clock::now();
  for (size_t first = 0; first < args.query_count;) {
    const size_t batch_count =
        std::min(args.query_batch_size, args.query_count - first);
    std::vector<std::vector<float>> queries;
    queries.reserve(batch_count);
    for (size_t offset = 0; offset < batch_count; ++offset) {
      const size_t query_id =
          (static_cast<std::uint64_t>(first + offset) * 104729ULL + args.seed) %
          corpus_count;
      query_ids->push_back(query_id);
      queries.push_back(MakeQuery(args, query_id));
    }
    std::vector<std::vector<kv::VectorResult>> batch_results;
    const auto start = std::chrono::steady_clock::now();
    const kv::Status status = db->SearchBatch(queries, args.top_k,
                                               args.ef_search, &batch_results);
    const auto end = std::chrono::steady_clock::now();
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return false;
    }
    if (batch_results.size() != batch_count) {
      std::cerr << "batch query returned an unexpected result count\n";
      return false;
    }
    timing->operations += batch_count;
    timing->per_operation_microseconds.push_back(
        std::chrono::duration<double, std::micro>(end - start).count() /
        static_cast<double>(batch_count));
    query_results->insert(query_results->end(),
                          std::make_move_iterator(batch_results.begin()),
                          std::make_move_iterator(batch_results.end()));
    first += batch_count;
  }
  timing->elapsed_seconds +=
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    benchmark_start)
          .count();
  return true;
}

std::vector<size_t> ExactTopK(const Args& args,
                              const std::vector<float>& query,
                              size_t corpus_count) {
  using Candidate = std::pair<float, size_t>;
  std::priority_queue<Candidate> nearest;
  std::vector<float> candidate(args.dimension);
  for (size_t id = 0; id < corpus_count; ++id) {
    for (size_t dimension = 0; dimension < args.dimension; ++dimension) {
      candidate[dimension] = RandomCoordinate(args.seed, id, dimension);
    }
    const float distance = kv::ComputeVectorDistanceUnchecked(
        query.data(), candidate.data(), args.dimension,
        kv::VectorDistanceMetric::kL2);
    if (nearest.size() < args.top_k) {
      nearest.emplace(distance, id);
    } else if (distance < nearest.top().first ||
               (distance == nearest.top().first && id < nearest.top().second)) {
      nearest.pop();
      nearest.emplace(distance, id);
    }
  }
  std::vector<size_t> result;
  result.reserve(nearest.size());
  while (!nearest.empty()) {
    result.push_back(nearest.top().second);
    nearest.pop();
  }
  return result;
}

double RecallAtK(const Args& args,
                 size_t corpus_count,
                 const std::vector<size_t>& query_ids,
                 const std::vector<std::vector<kv::VectorResult>>& results) {
  const size_t evaluated = std::min({args.recall_query_count, query_ids.size(),
                                     results.size()});
  if (evaluated == 0) {
    return -1.0;
  }
  size_t recalled = 0;
  for (size_t query_index = 0; query_index < evaluated; ++query_index) {
    const std::vector<size_t> exact = ExactTopK(
        args, MakeQuery(args, query_ids[query_index]), corpus_count);
    std::unordered_set<std::string> approximate_keys;
    for (const auto& result : results[query_index]) {
      approximate_keys.insert(result.key);
    }
    for (size_t exact_id : exact) {
      recalled += approximate_keys.count(KeyFor(exact_id));
    }
  }
  return static_cast<double>(recalled) /
         static_cast<double>(evaluated * std::min(args.top_k, corpus_count));
}

bool RunWrite(const Args& args) {
  kv::DB db(MakeOptions(args));
  if (!Open(&db)) {
    return false;
  }
  Timing timing;
  const bool ingested = Ingest(&db, args, 0, args.vector_count, &timing);
  const auto close_start = std::chrono::steady_clock::now();
  const kv::Status close_status = db.Close();
  const double close_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - close_start).count();
  timing.elapsed_seconds += close_seconds;
  if (!ingested || !close_status.ok()) {
    if (!close_status.ok()) {
      std::cerr << close_status.ToString() << '\n';
    }
    return false;
  }
  const StorageUsage usage = GetStorageUsage(args.path);
  std::cout << "mode=write\n";
  std::cout << "write_close_seconds=" << close_seconds << '\n';
  PrintTiming("write", timing);
  std::cout << "sstable_bytes=" << usage.sstable_bytes << '\n';
  std::cout << "index_bytes=" << usage.index_bytes << '\n';
  std::cout << "wal_bytes=" << usage.wal_bytes << '\n';
  return PrintMaintenance(args, db);
}

bool RunQuery(const Args& args) {
  if (!PopulateDatabase(args, args.vector_count)) {
    return false;
  }
  kv::DB db(MakeOptions(args));
  if (!Open(&db)) {
    return false;
  }
  Timing timing;
  std::vector<size_t> query_ids;
  std::vector<std::vector<kv::VectorResult>> results;
  const bool queried = TimedQueries(&db, args, args.vector_count, &timing,
                                    &query_ids, &results);
  const kv::Status close_status = db.Close();
  if (!queried || !close_status.ok()) {
    if (!close_status.ok()) {
      std::cerr << close_status.ToString() << '\n';
    }
    return false;
  }
  const double recall = RecallAtK(args, args.vector_count, query_ids, results);
  const StorageUsage usage = GetStorageUsage(args.path);
  std::cout << "mode=query\n";
  PrintTiming("query", timing);
  std::cout << "query_batch_size=" << args.query_batch_size << '\n';
  std::cout << "recall_query_count="
            << std::min(args.recall_query_count, args.query_count) << '\n';
  std::cout << "recall_at_" << args.top_k << '=' << recall << '\n';
  std::cout << "sstable_bytes=" << usage.sstable_bytes << '\n';
  std::cout << "index_bytes=" << usage.index_bytes << '\n';
  return true;
}

bool RunMixed(const Args& args) {
  const size_t warm_vectors = args.vector_count / 2;
  if (!PopulateDatabase(args, warm_vectors)) {
    return false;
  }
  kv::DB db(MakeOptions(args));
  if (!Open(&db)) {
    return false;
  }
  const size_t pair_count = args.mixed_operations / 2;
  Timing write_timing;
  Timing query_timing;
  const auto mixed_start = std::chrono::steady_clock::now();
  for (size_t first = 0; first < pair_count;) {
    const size_t count = std::min(
        {args.write_batch_size, args.query_batch_size, pair_count - first});
    if (!Ingest(&db, args, warm_vectors + first, count, &write_timing)) {
      return false;
    }
    std::vector<std::vector<float>> queries;
    queries.reserve(count);
    for (size_t offset = 0; offset < count; ++offset) {
      queries.push_back(MakeQuery(args, warm_vectors + first + offset));
    }
    std::vector<std::vector<kv::VectorResult>> results;
    const auto start = std::chrono::steady_clock::now();
    const kv::Status status = db.SearchBatch(queries, args.top_k,
                                              args.ef_search, &results);
    const auto end = std::chrono::steady_clock::now();
    if (!status.ok() || results.size() != count) {
      std::cerr << (status.ok() ? "mixed batch result count mismatch"
                                : status.ToString())
                << '\n';
      return false;
    }
    query_timing.operations += count;
    const double elapsed =
        std::chrono::duration<double>(end - start).count();
    query_timing.elapsed_seconds += elapsed;
    query_timing.per_operation_microseconds.push_back(
        elapsed * 1000000.0 / static_cast<double>(count));
    first += count;
  }
  const kv::Status close_status = db.Close();
  if (!close_status.ok()) {
    std::cerr << close_status.ToString() << '\n';
    return false;
  }
  const StorageUsage usage = GetStorageUsage(args.path);
  std::cout << "mode=mixed\n";
  const double mixed_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - mixed_start).count();
  std::cout << "mixed_wall_seconds=" << mixed_seconds << '\n';
  std::cout << "mixed_wall_write_per_second=" << pair_count / mixed_seconds << '\n';
  std::cout << "mixed_wall_query_per_second=" << pair_count / mixed_seconds << '\n';
  PrintTiming("mixed_write", write_timing);
  PrintTiming("mixed_query", query_timing);
  std::cout << "mixed_write_fraction=0.5\n";
  std::cout << "mixed_query_fraction=0.5\n";
  std::cout << "sstable_bytes=" << usage.sstable_bytes << '\n';
  std::cout << "index_bytes=" << usage.index_bytes << '\n';
  return PrintMaintenance(args, db);
}

bool ResetPath(const Args& args) {
  if (!args.reset) {
    if (std::filesystem::exists(args.path) &&
        !std::filesystem::is_empty(args.path)) {
      std::cerr << "benchmark path is not empty; use a fresh path or --reset\n";
      return false;
    }
    return true;
  }
  std::error_code error;
  std::filesystem::remove_all(args.path, error);
  if (error) {
    std::cerr << "failed to reset benchmark path: " << error.message() << '\n';
    return false;
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
  std::cout << std::fixed << std::setprecision(6);
  std::cout << "vectors=" << args.vector_count << '\n';
  std::cout << "dimension=" << args.dimension << '\n';
  std::cout << "memtable_entries_limit=" << args.memtable_limit << '\n';
  std::cout << "level0_sstable_limit=" << args.l0_limit << '\n';
  std::cout << "level1_size_limit_bytes=" << args.l1_bytes << '\n';
  std::cout << "block_cache_capacity=" << args.block_cache_capacity << '\n';
  std::cout << "hnsw_m=" << args.hnsw_m << '\n';
  std::cout << "ef_construction=" << args.ef_construction << '\n';
  std::cout << "ef_search=" << args.ef_search << '\n';
  std::cout << "seed=" << args.seed << '\n';
  std::cout << "durability=wal_sync_per_batch\n";
  std::cout << "distance_backend=" << kv::VectorDistanceBackend() << '\n';
  std::cout << "avx2_enabled="
            << (kv::VectorDistanceUsesAVX2() ? "true" : "false") << '\n';
  if (!ResetPath(args)) {
    return 1;
  }

  if (args.mode == "write") {
    return RunWrite(args) ? 0 : 1;
  }
  if (args.mode == "query") {
    return RunQuery(args) ? 0 : 1;
  }
  if (args.mode == "mixed") {
    return RunMixed(args) ? 0 : 1;
  }

  const std::filesystem::path root = args.path;
  args.path = root / "write";
  if (!ResetPath(args) || !RunWrite(args)) {
    return 1;
  }
  args.path = root / "query";
  if (!ResetPath(args) || !RunQuery(args)) {
    return 1;
  }
  args.path = root / "mixed";
  return ResetPath(args) && RunMixed(args) ? 0 : 1;
}
