#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hnsw_index.h"

namespace {

[[noreturn]] void CheckFailed(const char* expression,
                              const char* file,
                              int line) {
  std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
  std::abort();
}

#define CHECK(expression)                              \
  do {                                                 \
    if (!(expression)) {                               \
      CheckFailed(#expression, __FILE__, __LINE__);    \
    }                                                  \
  } while (false)

void MustOK(const kv::Status& status) {
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    std::abort();
  }
}

void TestValidationAndBasicSearch() {
  kv::HNSWOptions invalid_options;
  kv::HNSWIndex invalid(invalid_options);
  CHECK(!invalid.Insert("bad", {1.0f}).ok());

  kv::HNSWOptions options;
  options.dimension = 2;
  options.max_neighbors = 4;
  options.ef_construction = 24;
  options.random_seed = 7;

  kv::HNSWOptions invalid_metric_options = options;
  invalid_metric_options.metric = static_cast<kv::VectorDistanceMetric>(99);
  kv::HNSWIndex invalid_metric(invalid_metric_options);
  CHECK(!invalid_metric.Insert("bad-metric", {1.0f, 0.0f}).ok());

  kv::HNSWIndex index(options);

  std::vector<kv::VectorResult> results;
  MustOK(index.Search({0.0f, 0.0f}, 3, 8, &results));
  CHECK(results.empty());

  MustOK(index.Insert("origin", {0.0f, 0.0f}, "origin-metadata"));
  MustOK(index.Insert("east", {1.0f, 0.0f}, "east-metadata"));
  MustOK(index.Insert("north", {0.0f, 2.0f}, "north-metadata"));
  MustOK(index.Insert("far", {8.0f, 8.0f}));
  CHECK(index.Size() == 4);

  CHECK(!index.Insert("origin", {3.0f, 3.0f}).ok());
  CHECK(!index.Insert("wrong-dimension", {1.0f}).ok());
  CHECK(!index.Insert("not-finite",
                      {std::numeric_limits<float>::infinity(), 0.0f}).ok());
  CHECK(!index.Search({0.0f, 0.0f}, 1, 0, &results).ok());
  CHECK(!index.Search({0.0f}, 1, 8, &results).ok());

  MustOK(index.Search({0.1f, 0.0f}, 3, 8, &results));
  CHECK(results.size() == 3);
  CHECK(results[0].key == "origin");
  CHECK(results[0].metadata == "origin-metadata");
  CHECK(results[1].key == "east");
  CHECK(results[0].distance <= results[1].distance);
  CHECK(results[1].distance <= results[2].distance);

  const kv::HNSWStats stats = index.Stats();
  CHECK(stats.node_count == 4);
  CHECK(stats.max_level >= 0);
  CHECK(stats.nodes_per_level[0] == 4);
  CHECK(stats.max_neighbor_count <= options.max_neighbors);

  kv::HNSWOptions cosine_options = options;
  cosine_options.metric = kv::VectorDistanceMetric::kCosine;
  kv::HNSWIndex cosine(cosine_options);
  CHECK(!cosine.Insert("zero", {0.0f, 0.0f}).ok());
  MustOK(cosine.Insert("x", {1.0f, 0.0f}));
  MustOK(cosine.Insert("y", {0.0f, 1.0f}));
  MustOK(cosine.Search({0.9f, 0.1f}, 1, 4, &results));
  CHECK(results.size() == 1);
  CHECK(results[0].key == "x");
}

std::vector<size_t> ExactTopK(
    const std::vector<std::vector<float>>& vectors,
    const std::vector<float>& query,
    size_t top_k) {
  std::vector<std::pair<float, size_t>> distances;
  distances.reserve(vectors.size());
  for (size_t i = 0; i < vectors.size(); ++i) {
    float distance = 0.0f;
    MustOK(kv::ComputeVectorDistance(
        query, vectors[i], kv::VectorDistanceMetric::kL2, &distance));
    distances.emplace_back(distance, i);
  }
  std::partial_sort(distances.begin(), distances.begin() + top_k,
                    distances.end());

  std::vector<size_t> result;
  result.reserve(top_k);
  for (size_t i = 0; i < top_k; ++i) {
    result.push_back(distances[i].second);
  }
  return result;
}

void TestRecallAndLatencyAcceptance() {
  constexpr size_t kVectorCount = 1000;
  constexpr size_t kDimension = 128;
  constexpr size_t kClusterCount = 20;
  constexpr size_t kQueryCount = 100;
  constexpr size_t kTopK = 10;

  std::mt19937 random(20260901);
  std::normal_distribution<float> center_distribution(0.0f, 1.0f);
  std::normal_distribution<float> point_noise(0.0f, 0.10f);
  std::normal_distribution<float> query_noise(0.0f, 0.02f);

  std::vector<std::vector<float>> centers(
      kClusterCount, std::vector<float>(kDimension));
  for (auto& center : centers) {
    for (float& value : center) {
      value = center_distribution(random);
    }
  }

  std::vector<std::vector<float>> vectors(
      kVectorCount, std::vector<float>(kDimension));
  for (size_t i = 0; i < kVectorCount; ++i) {
    const auto& center = centers[i % kClusterCount];
    for (size_t dimension = 0; dimension < kDimension; ++dimension) {
      vectors[i][dimension] = center[dimension] + point_noise(random);
    }
  }

  kv::HNSWOptions options;
  options.dimension = kDimension;
  options.max_neighbors = 16;
  options.ef_construction = 200;
  options.random_seed = 20260901;
  kv::HNSWIndex index(options);
  for (size_t i = 0; i < vectors.size(); ++i) {
    MustOK(index.Insert("point:" + std::to_string(i), vectors[i]));
  }

  const kv::HNSWStats stats = index.Stats();
  CHECK(stats.node_count == kVectorCount);
  CHECK(stats.max_level >= 1);
  CHECK(stats.nodes_per_level[0] == kVectorCount);
  CHECK(stats.directed_edge_count > kVectorCount);
  CHECK(stats.max_neighbor_count <= options.max_neighbors);
  for (size_t level = 1; level < stats.nodes_per_level.size(); ++level) {
    CHECK(stats.nodes_per_level[level] <= stats.nodes_per_level[level - 1]);
  }

  std::vector<std::vector<float>> queries;
  queries.reserve(kQueryCount);
  for (size_t i = 0; i < kQueryCount; ++i) {
    std::vector<float> query = vectors[(i * 37) % vectors.size()];
    for (float& value : query) {
      value += query_noise(random);
    }
    queries.push_back(std::move(query));
  }

  size_t recalled = 0;
  std::vector<kv::VectorResult> approximate;
  const auto search_start = std::chrono::steady_clock::now();
  for (const auto& query : queries) {
    MustOK(index.Search(query, kTopK, 100, &approximate));
  }
  const auto search_elapsed = std::chrono::steady_clock::now() - search_start;

  for (const auto& query : queries) {
    const auto exact = ExactTopK(vectors, query, kTopK);
    std::unordered_set<std::string> exact_keys;
    for (size_t id : exact) {
      exact_keys.insert("point:" + std::to_string(id));
    }
    MustOK(index.Search(query, kTopK, 100, &approximate));
    CHECK(approximate.size() == kTopK);
    for (const auto& result : approximate) {
      recalled += exact_keys.count(result.key);
    }
  }

  const double recall = static_cast<double>(recalled) /
      static_cast<double>(kQueryCount * kTopK);
  const double average_micros =
      std::chrono::duration<double, std::micro>(search_elapsed).count() /
      static_cast<double>(kQueryCount);
  std::cout << "HNSW acceptance: Recall@10=" << recall
            << ", average_search_us=" << average_micros << '\n';
  CHECK(recall > 0.90);
  CHECK(average_micros < 1000.0);
}

}  // namespace

int main() {
  TestValidationAndBasicSearch();
  TestRecallAndLatencyAcceptance();
  std::cout << "hnsw_test passed\n";
  return 0;
}
