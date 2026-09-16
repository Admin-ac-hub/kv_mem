#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hnsw_index.h"
#include "format.h"

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

  std::vector<std::vector<kv::VectorResult>> batch_results;
  MustOK(index.SearchBatch({{0.1f, 0.0f}, {0.0f, 1.9f}}, 2, 8,
                            &batch_results));
  CHECK(batch_results.size() == 2);
  CHECK(batch_results[0].size() == 2);
  CHECK(batch_results[0][0].key == "origin");
  CHECK(batch_results[1].size() == 2);
  CHECK(batch_results[1][0].key == "north");
  CHECK(!index.SearchBatch({{0.0f}}, 2, 8, &batch_results).ok());

  const kv::HNSWStats stats = index.Stats();
  CHECK(stats.node_count == 4);
  CHECK(stats.max_level >= 0);
  CHECK(stats.nodes_per_level[0] == 4);
  CHECK(stats.max_neighbor_count <= 2 * options.max_neighbors);

  kv::HNSWOptions cosine_options = options;
  cosine_options.metric = kv::VectorDistanceMetric::kCosine;
  kv::HNSWIndex cosine(cosine_options);
  CHECK(!cosine.Insert("zero", {0.0f, 0.0f}).ok());
  MustOK(cosine.Insert("x", {1.0f, 0.0f}));
  MustOK(cosine.Insert("y", {0.0f, 1.0f}));
  MustOK(cosine.Search({0.9f, 0.1f}, 1, 4, &results));
  CHECK(results.size() == 1);
  CHECK(results[0].key == "x");

  std::vector<float> lhs(17);
  std::vector<float> rhs(17);
  double squared_l2 = 0.0;
  for (size_t i = 0; i < lhs.size(); ++i) {
    lhs[i] = static_cast<float>(i) * 0.25f;
    rhs[i] = static_cast<float>(i) * -0.125f;
    const double difference = static_cast<double>(lhs[i]) - rhs[i];
    squared_l2 += difference * difference;
  }
  float l2_distance = 0.0f;
  MustOK(kv::ComputeVectorDistance(lhs, rhs, kv::VectorDistanceMetric::kL2,
                                   &l2_distance));
  CHECK(std::fabs(l2_distance - std::sqrt(squared_l2)) < 1e-5f);
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

void TestClusteredRecallRegression() {
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
  CHECK(stats.max_neighbor_count > options.max_neighbors);
  CHECK(stats.max_neighbors_per_level[0] <= 2 * options.max_neighbors);
  for (size_t level = 1; level < stats.nodes_per_level.size(); ++level) {
    CHECK(stats.max_neighbors_per_level[level] <= options.max_neighbors);
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
  std::vector<std::vector<kv::VectorResult>> batch_results;
  MustOK(index.SearchBatch(queries, kTopK, 100, &batch_results));
  const auto search_elapsed = std::chrono::steady_clock::now() - search_start;
  CHECK(batch_results.size() == queries.size());
  for (const auto& results : batch_results) {
    CHECK(results.size() == kTopK);
  }

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
  std::cout << "HNSW small-data regression: Recall@10=" << recall
            << ", average_search_us=" << average_micros << '\n';
  CHECK(recall > 0.95);
}

void TestPersistenceAndMerge() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "mini_lsm_kv_hnsw_persistence";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  kv::HNSWOptions options;
  options.dimension = 3;
  options.max_neighbors = 8;
  options.ef_construction = 32;
  options.random_seed = 17;
  kv::HNSWIndex index(options);
  MustOK(index.InsertVersion("old", 1, {1.0f, 0.0f, 0.0f}, "old-meta"));
  MustOK(index.InsertVersion("new", 2, {0.0f, 1.0f, 0.0f}, "new-meta"));
  MustOK(index.MarkDeleted("gone", 3));

  const auto first_path = root / "index_000001.hnsw";
  MustOK(index.Save(first_path));

  kv::HNSWIndex restored(options);
  MustOK(restored.Load(first_path));
  CHECK(restored.Size() == index.Size());
  std::vector<kv::VectorResult> results;
  MustOK(restored.SearchAtSequence({1.0f, 0.0f, 0.0f}, 2, 16, 1,
                                   &results));
  CHECK(results.size() == 1);
  CHECK(results[0].key == "old");
  CHECK(results[0].metadata == "old-meta");

  // The small graph obeys the old M limit, so changing only the header creates
  // a valid v1 fixture (the body encoding and checksum are unchanged).
  std::ifstream saved(first_path, std::ios::binary);
  std::string legacy((std::istreambuf_iterator<char>(saved)), {});
  kv::EncodeFixed32(1, legacy.data() + 8);
  const auto legacy_path = root / "legacy.hnsw";
  {
    std::ofstream output(legacy_path, std::ios::binary);
    output.write(legacy.data(), legacy.size());
  }
  kv::HNSWIndex legacy_index(options);
  MustOK(legacy_index.Load(legacy_path));
  MustOK(legacy_index.SearchAtSequence({1.0f, 0.0f, 0.0f}, 2, 16, 1,
                                      &results));
  CHECK(results.size() == 1 && results[0].key == "old");

  kv::HNSWIndex second(options);
  MustOK(second.InsertVersion("third", 4, {0.0f, 0.0f, 1.0f}, "third-meta"));
  const auto second_path = root / "index_000002.hnsw";
  MustOK(second.Save(second_path));
  MustOK(restored.MergeFrom(second_path));
  MustOK(restored.Search({0.0f, 0.0f, 1.0f}, 1, 16, &results));
  CHECK(results.size() == 1);
  CHECK(results[0].key == "third");

  const auto corrupt_path = root / "corrupt.hnsw";
  std::filesystem::copy_file(first_path, corrupt_path);
  std::fstream corrupt(corrupt_path,
                       std::ios::binary | std::ios::in | std::ios::out);
  corrupt.seekp(-1, std::ios::end);
  char byte = 0;
  corrupt.get(byte);
  corrupt.seekp(-1, std::ios::end);
  corrupt.put(static_cast<char>(byte ^ 0x01));
  corrupt.close();
  kv::HNSWIndex invalid(options);
  CHECK(!invalid.Load(corrupt_path).ok());

  std::filesystem::remove_all(root);
}

void TestDistanceBackendAgainstReference() {
  std::mt19937 random(103);
  std::uniform_real_distribution<float> coordinate(-1.0f, 1.0f);
  for (size_t dimension : {1, 2, 3, 4, 7, 8, 17, 128, 768, 1025}) {
    for (float scale : {1e-10f, 1.0f, 1e10f}) {
      std::vector<float> lhs(dimension), rhs(dimension);
      double l2 = 0, dot = 0, left_norm = 0, right_norm = 0;
      for (size_t i = 0; i < dimension; ++i) {
        lhs[i] = coordinate(random) * scale;
        rhs[i] = coordinate(random) * scale;
        const double l = lhs[i], r = rhs[i];
        l2 += (l - r) * (l - r);
        dot += l * r;
        left_norm += l * l;
        right_norm += r * r;
      }
      for (auto metric : {kv::VectorDistanceMetric::kL2,
                           kv::VectorDistanceMetric::kInnerProduct,
                           kv::VectorDistanceMetric::kCosine}) {
        const double expected = metric == kv::VectorDistanceMetric::kL2
            ? std::sqrt(l2)
            : metric == kv::VectorDistanceMetric::kInnerProduct
                ? -dot : 1 - std::clamp(dot / std::sqrt(left_norm * right_norm), -1.0, 1.0);
        float actual = 0;
        MustOK(kv::ComputeVectorDistance(lhs, rhs, metric, &actual));
        CHECK(std::fabs(actual - expected) <= 1e-5 * std::max(1.0, std::fabs(expected)));
      }
    }
  }
}

void TestGraphFileValidation() {
  const auto root = std::filesystem::temp_directory_path() /
                    "mini_lsm_kv_hnsw_validation";
  std::filesystem::create_directories(root);
  kv::HNSWOptions options;
  options.dimension = 3;
  options.max_neighbors = 4;
  kv::HNSWIndex index(options);
  std::mt19937 random(91);
  std::normal_distribution<float> coordinate;
  for (size_t i = 0; i < 200; ++i) {
    MustOK(index.Insert(std::to_string(i),
                        {coordinate(random), coordinate(random), coordinate(random)}));
  }
  CHECK(index.Stats().max_neighbor_count > options.max_neighbors);
  const auto path = root / "graph.hnsw";
  MustOK(index.Save(path));
  kv::HNSWIndex restored(options);
  MustOK(restored.Load(path));
  CHECK(restored.Stats().max_neighbors_per_level ==
        index.Stats().max_neighbors_per_level);
  std::vector<kv::VectorResult> before, after;
  MustOK(index.Search({0, 0, 0}, 10, 64, &before));
  MustOK(restored.Search({0, 0, 0}, 10, 64, &after));
  CHECK(before.size() == after.size());
  for (size_t i = 0; i < before.size(); ++i) {
    CHECK(before[i].key == after[i].key);
    CHECK(before[i].distance == after[i].distance);
  }

  std::ifstream input(path, std::ios::binary);
  const std::string valid((std::istreambuf_iterator<char>(input)), {});
  std::vector<size_t> layers;
  std::vector<std::vector<size_t>> neighbor_offsets;
  size_t offset = 56;
  for (size_t i = 0; i < 200; ++i) {
    offset += 8;
    const size_t key_size = kv::DecodeFixed32(valid.data() + offset);
    offset += 4 + key_size + 8 + 4 * options.dimension;
    const size_t metadata_size = kv::DecodeFixed32(valid.data() + offset);
    offset += 4 + metadata_size;
    const size_t layer_count = kv::DecodeFixed32(valid.data() + offset);
    offset += 4;
    layers.push_back(layer_count);
    neighbor_offsets.emplace_back();
    for (size_t layer = 0; layer < layer_count; ++layer) {
      const size_t count = kv::DecodeFixed32(valid.data() + offset);
      offset += 4;
      neighbor_offsets.back().push_back(count == 0 ? 0 : offset);
      offset += 8 * count;
    }
  }
  const auto rejects = [&](std::string corrupt) {
    // Recompute CRC so these exercise structural validation, not just CRC.
    kv::EncodeFixed32(kv::CRC32(std::string_view(corrupt).substr(
                          56, corrupt.size() - 60)),
                      corrupt.data() + corrupt.size() - 4);
    const auto bad_path = root / "bad.hnsw";
    {
      std::ofstream output(bad_path, std::ios::binary);
      output.write(corrupt.data(), corrupt.size());
    }
    CHECK(restored.Load(bad_path).IsCorruption());
    // A rejected load must leave the old, searchable graph intact.
    MustOK(restored.Search({0, 0, 0}, 10, 64, &after));
    CHECK(after[0].key == before[0].key);
    CHECK(restored.Size() == 200);
  };
  auto corrupt = valid;
  kv::EncodeFixed32(63, corrupt.data() + 28);
  rejects(corrupt);
  corrupt = valid;
  kv::EncodeFixed32(std::numeric_limits<std::uint32_t>::max(), corrupt.data() + 28);
  rejects(corrupt);
  corrupt = valid;
  kv::EncodeFixed64(std::numeric_limits<std::uint64_t>::max(), corrupt.data() + 32);
  rejects(corrupt);
  const size_t low_node = std::find(layers.begin(), layers.end(), 1) - layers.begin();
  CHECK(low_node < layers.size());
  corrupt = valid;
  kv::EncodeFixed64(low_node, corrupt.data() + 48);
  rejects(corrupt);
  bool tested_upper_edge = false;
  for (size_t i = 0; i < layers.size(); ++i) {
    if (layers[i] > 1 && neighbor_offsets[i][1] != 0) {
      corrupt = valid;
      kv::EncodeFixed64(low_node, corrupt.data() + neighbor_offsets[i][1]);
      rejects(corrupt);
      tested_upper_edge = true;
      break;
    }
  }
  CHECK(tested_upper_edge);
  corrupt = valid;
  CHECK(neighbor_offsets[0][0] != 0);
  kv::EncodeFixed64(0, corrupt.data() + neighbor_offsets[0][0]);
  rejects(corrupt);
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  TestValidationAndBasicSearch();
  TestClusteredRecallRegression();
  TestPersistenceAndMerge();
  TestGraphFileValidation();
  TestDistanceBackendAgainstReference();
  std::cout << "hnsw_test passed\n";
  return 0;
}
