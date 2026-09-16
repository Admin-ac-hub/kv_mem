// HNSW recall / insert-throughput probe.
//
// Standalone diagnostic that exercises HNSWIndex directly, with no LSM layer,
// no LSM work (HNSW version bookkeeping/visibility still applies). It answers
// two questions the end-to-end vector benchmark cannot separate:
//
//   1. Is a low Recall@10 caused by the HNSW graph itself, or by the LSM
//      integration (visibility filtering, duplicate nodes, reindexing)?
//   2. How much of the write-path latency is HNSW insertion versus WAL/MemTable/
//      flush/compaction?
//
// Build:
//   cmake -S . -B build-week6-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
//   cmake --build build-week6-bench --parallel --target hnsw_recall_probe
//
// Usage:
//   hnsw_recall_probe recall  <n> <dim> <M> <ef_construction>
//   hnsw_recall_probe insert  <n> <dim> <M> <ef_construction>
//
// `recall` reports Recall@10 against exact brute force on both an i.i.d.
// Gaussian corpus and a clustered corpus (200 centroids, 0.05 noise), at
// ef_search 16/32/64/128/256/512. `insert` times the index build alone.
// `regression` requires Recall@10 > 0.95 at ef=512 for iid and shared-center
// clustered queries. Independent-center clustered queries are reported as OOD.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "hnsw_index.h"

namespace {

using kv::HNSWIndex;
using kv::HNSWOptions;
using kv::VectorDistanceMetric;
using kv::VectorResult;

using Corpus = std::vector<std::vector<float>>;

void MustOK(const kv::Status& status) {
  if (!status.ok()) throw std::runtime_error(status.ToString());
}

Corpus IidGaussian(size_t n, size_t dim, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  Corpus out(n, std::vector<float>(dim));
  for (auto& v : out) {
    for (auto& x : v) {
      x = dist(rng);
    }
  }
  return out;
}

Corpus Clustered(size_t n, size_t dim, size_t clusters,
                 std::uint64_t center_seed, std::uint64_t noise_seed) {
  std::mt19937_64 rng(center_seed);
  std::normal_distribution<float> center_dist(0.0f, 1.0f);
  std::normal_distribution<float> noise(0.0f, 0.05f);
  Corpus centers(clusters, std::vector<float>(dim));
  for (auto& c : centers) {
    for (auto& x : c) {
      x = center_dist(rng);
    }
  }
  rng.seed(noise_seed);
  Corpus out(n, std::vector<float>(dim));
  for (size_t i = 0; i < n; ++i) {
    const auto& c = centers[i % clusters];
    for (size_t d = 0; d < dim; ++d) {
      out[i][d] = c[d] + noise(rng);
    }
  }
  return out;
}

std::vector<std::string> BruteTopK(const Corpus& data,
                                   const std::vector<float>& query,
                                   size_t k) {
  std::vector<std::pair<float, size_t>> scored;
  scored.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    scored.emplace_back(kv::ComputeVectorDistanceUnchecked(
        query.data(), data[i].data(), query.size(), VectorDistanceMetric::kL2), i);
  }
  std::partial_sort(scored.begin(), scored.begin() + k, scored.end());
  std::vector<std::string> out;
  out.reserve(k);
  for (size_t i = 0; i < k; ++i) {
    out.push_back("k" + std::to_string(scored[i].second));
  }
  return out;
}

double ReportRecall(const char* label,
                  const Corpus& data,
                  const Corpus& queries,
                  size_t dim,
                  size_t max_neighbors,
                  size_t ef_construction,
                  const Corpus* ood_queries = nullptr) {
  HNSWOptions options;
  options.dimension = dim;
  options.max_neighbors = max_neighbors;
  options.ef_construction = ef_construction;
  options.metric = VectorDistanceMetric::kL2;

  HNSWIndex index(options);
  const auto build_start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < data.size(); ++i) {
    MustOK(index.Insert("k" + std::to_string(i), data[i]));
  }

  const kv::HNSWStats stats = index.Stats();
  std::printf("%s | n=%zu dim=%zu M=%zu ef_c=%zu | nodes=%zu max_level=%d "
              "directed_edges=%zu max_degree=%zu avg_degree=%.1f\n",
              label, data.size(), dim, max_neighbors, ef_construction,
              stats.node_count, stats.max_level, stats.directed_edge_count,
              stats.max_neighbor_count,
              static_cast<double>(stats.directed_edge_count) /
                  static_cast<double>(stats.node_count));
  std::printf("    build_seconds=%.3f queries=%zu\n",
      std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count(),
      queries.size());
  std::fflush(stdout);

  const auto evaluate = [&](const char* query_label, const Corpus& query_set) {
    std::vector<std::set<std::string>> truth_sets;
    for (const auto& query : query_set) {
      const auto truth = BruteTopK(data, query, 10);
      truth_sets.emplace_back(truth.begin(), truth.end());
    }
    double final_recall = 0.0;
    for (size_t ef : {size_t(16), size_t(32), size_t(64), size_t(128),
                      size_t(256), size_t(512)}) {
      double recall = 0.0;
      size_t duplicate_keys = 0;
      size_t query_id = 0;
      const auto query_start = std::chrono::steady_clock::now();
      for (const auto& query : query_set) {
        std::vector<VectorResult> results;
        MustOK(index.Search(query, 10, ef, &results));
        const auto& truth_set = truth_sets[query_id++];
        std::set<std::string> got;
        for (const auto& result : results) {
          if (!got.insert(result.key).second) {
            ++duplicate_keys;
          }
        }
        size_t hits = 0;
        for (const auto& key : got) {
          if (truth_set.count(key) != 0) {
            ++hits;
          }
        }
        recall += static_cast<double>(hits) / 10.0;
      }
      final_recall = recall / static_cast<double>(query_set.size());
      const double query_seconds = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - query_start).count();
      std::printf("    %s ef=%-4zu recall@10=%.4f duplicate_keys=%zu qps=%.1f\n",
                  query_label, ef, final_recall, duplicate_keys,
                  query_set.size() / query_seconds);
      std::fflush(stdout);
      if (duplicate_keys != 0) throw std::runtime_error("duplicate result keys");
    }
    return final_recall;
  };
  const double recall = evaluate("in_distribution", queries);
  if (ood_queries != nullptr) evaluate("independent_centers_OOD", *ood_queries);
  return recall;
}

int RunRecall(size_t n, size_t dim, size_t max_neighbors,
              size_t ef_construction, size_t query_count, bool regression) {
  const double iid_recall = ReportRecall("i.i.d. Gaussian", IidGaussian(n, dim, 12345),
               IidGaussian(query_count, dim, 999), dim, max_neighbors,
               ef_construction);
  const auto ood = Clustered(query_count, dim, 200, 999, 1000);
  const double clustered_recall = ReportRecall(
      "clustered", Clustered(n, dim, 200, 12345, 23456),
      Clustered(query_count, dim, 200, 12345, 999), dim, max_neighbors,
      ef_construction, &ood);
  if (regression && (iid_recall <= 0.95 || clustered_recall <= 0.95)) {
    std::fprintf(stderr, "Recall@10 regression: expected >0.95 at ef=512\n");
    return 1;
  }
  return 0;
}

int RunInsert(size_t n, size_t dim, size_t max_neighbors,
              size_t ef_construction) {
  const Corpus data = IidGaussian(n, dim, 7);
  HNSWOptions options;
  options.dimension = dim;
  options.max_neighbors = max_neighbors;
  options.ef_construction = ef_construction;
  options.metric = VectorDistanceMetric::kL2;

  HNSWIndex index(options);
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < n; ++i) {
    MustOK(index.Insert("k" + std::to_string(i), data[i]));
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  std::printf("n=%zu dim=%zu M=%zu ef_c=%zu | insert %.2fs -> %.1f inserts/s "
              "(%.0f us/insert)\n",
              n, dim, max_neighbors, ef_construction, seconds,
              static_cast<double>(n) / seconds, seconds * 1e6 /
                                                     static_cast<double>(n));
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  std::printf("distance_backend=%s\n", kv::VectorDistanceBackend());
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <recall|insert|regression> [n] [dim] [M] [ef_construction] [queries]\n",
                 argv[0]);
    return 2;
  }
  const std::string_view mode = argv[1];
  const auto parse = [&](int position, size_t fallback) {
    if (argc <= position) return fallback;
    const std::string value(argv[position]);
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
      throw std::invalid_argument("expected a nonnegative integer");
    return static_cast<size_t>(std::stoull(value));
  };
  const size_t n = parse(2, 20000);
  const size_t dim = parse(3, 768);
  const size_t max_neighbors = parse(4, 16);
  const size_t ef_construction = parse(5, 200);
  const size_t query_count = parse(6, 100);
  if (argc > 7 || n < 10 || dim == 0 || max_neighbors < 2 ||
      ef_construction == 0 || query_count == 0) {
    throw std::invalid_argument("n>=10, dim>0, M>=2, ef_construction>0, queries>0 required");
  }

  if (mode == "recall" || mode == "regression") {
    return RunRecall(n, dim, max_neighbors, ef_construction, query_count,
                     mode == "regression");
  }
  if (mode == "insert") {
    return RunInsert(n, dim, max_neighbors, ef_construction);
  }
  std::fprintf(stderr, "unknown mode: %s\n", std::string(mode).c_str());
  return 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 2;
}
