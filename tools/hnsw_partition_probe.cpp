// Stage-0 go/no-go probe for docs/VECTOR_INDEX_ARCHITECTURE.md.
//
// The architecture proposal replaces one global HNSW graph with P independent
// per-writer segments, then answers every query by searching all P segments and
// merging. That is only worth building if the merged recall still matches a
// single graph at the same per-segment search budget. This probe measures
// exactly that, before any production code is touched.
//
// It also measures the distance-kernel rate, so the FLOP argument in
// section 1.3 of the design document can be checked against a measured
// numerator instead of a modelled one.
//
// Build:
//   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKV_ENABLE_VECTOR_EXTENSION=ON
//   cmake --build build --parallel --target hnsw_partition_probe
//
// Usage:
//   hnsw_partition_probe partition [n] [dim] [M] [ef_c] [queries] [P]
//   hnsw_partition_probe kernel    [dim] [iterations] [stream|hnsw|hot]
//
// `partition` builds one reference graph over the whole corpus and P graphs
// over a fixed random 1/P partition of the same corpus. It then prints two
// independent Recall@10 curves -- the single graph over ef 128/256/512, and the
// merged fan-out over per-segment ef 16/32/64/128/256/512 -- for three query
// distributions. The merged QPS is the sequential fan-out figure; a parallel
// fan-out over P workers would be faster by roughly a factor of P, because the
// segments are equal-sized.
//
// `kernel` reports the pure distance-kernel throughput in GFLOP/s, using the
// same 2 * dim FLOP convention as the design document.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
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

constexpr size_t kTopK = 10;
// The single graph is swept over the ef values used by the existing CI gate.
const std::vector<size_t> kSingleEfSweep = {128, 256, 512};
// Segments are swept over a wider range, including values below the single
// graph's range, so the iso-recall cost of partitioning is observable.
const std::vector<size_t> kSegmentEfSweep = {16, 32, 64, 128, 256, 512};

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

Corpus Clustered(size_t n, size_t dim, size_t clusters, std::uint64_t center_seed,
                 std::uint64_t noise_seed) {
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

HNSWOptions MakeOptions(size_t dim, size_t max_neighbors,
                        size_t ef_construction) {
  HNSWOptions options;
  options.dimension = dim;
  options.max_neighbors = max_neighbors;
  options.ef_construction = ef_construction;
  options.metric = VectorDistanceMetric::kL2;
  return options;
}

std::string KeyFor(size_t index) { return "k" + std::to_string(index); }

// Ground truth over the whole corpus, independent of any index.
std::vector<std::set<std::string>> BruteTruth(const Corpus& data,
                                              const Corpus& queries) {
  std::vector<std::set<std::string>> truth;
  truth.reserve(queries.size());
  std::vector<std::pair<float, size_t>> scored;
  for (const auto& query : queries) {
    scored.clear();
    scored.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
      scored.emplace_back(
          kv::ComputeVectorDistanceUnchecked(query.data(), data[i].data(),
                                             query.size(),
                                             VectorDistanceMetric::kL2),
          i);
    }
    std::partial_sort(scored.begin(), scored.begin() + kTopK, scored.end());
    std::set<std::string> keys;
    for (size_t i = 0; i < kTopK; ++i) {
      keys.insert(KeyFor(scored[i].second));
    }
    truth.push_back(std::move(keys));
  }
  return truth;
}

double RecallOf(const std::vector<VectorResult>& results,
                const std::set<std::string>& truth) {
  size_t hits = 0;
  for (const auto& result : results) {
    if (truth.count(result.key) != 0) {
      ++hits;
    }
  }
  return static_cast<double>(hits) / static_cast<double>(kTopK);
}

// An arm is one or more independent HNSWIndex instances. Using the existing
// public API keeps this probe free of production-code changes: a segment in the
// design document is structurally identical to a standalone HNSWIndex with its
// own node ids and its own entry point.
struct Arm {
  std::vector<std::unique_ptr<HNSWIndex>> segments;
  size_t node_count = 0;
};

// Search every segment and merge by distance. Keys are unique in this probe, so
// merging is a plain top-k selection; version arbitration is out of scope here.
std::vector<VectorResult> FanOutSearch(const Arm& arm,
                                       const std::vector<float>& query,
                                       size_t ef) {
  if (arm.segments.size() == 1) {
    std::vector<VectorResult> results;
    MustOK(arm.segments.front()->Search(query, kTopK, ef, &results));
    return results;
  }
  std::vector<VectorResult> merged;
  std::vector<VectorResult> partial;
  for (const auto& segment : arm.segments) {
    partial.clear();
    MustOK(segment->Search(query, kTopK, ef, &partial));
    merged.insert(merged.end(), partial.begin(), partial.end());
  }
  std::sort(merged.begin(), merged.end(),
            [](const VectorResult& lhs, const VectorResult& rhs) {
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              return lhs.key < rhs.key;
            });
  if (merged.size() > kTopK) {
    merged.resize(kTopK);
  }
  return merged;
}

Arm BuildSingle(const Corpus& data, const HNSWOptions& options) {
  Arm arm;
  arm.segments.push_back(std::make_unique<HNSWIndex>(options));
  for (size_t i = 0; i < data.size(); ++i) {
    MustOK(arm.segments.front()->Insert(KeyFor(i), data[i]));
  }
  arm.node_count = data.size();
  return arm;
}

// P graphs over a fixed random partition of the corpus. A random partition is
// the fair model of both candidate routing schemes: thread-local routing
// assigns vectors to segments by whichever thread inserted them, and
// hash(key) % P assigns them by key hash. Neither is correlated with the
// corpus geometry, so a shuffled assignment is the right model for recall.
Arm BuildPartitioned(const Corpus& data, const HNSWOptions& options, size_t p) {
  std::vector<size_t> assignment(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    assignment[i] = i % p;
  }
  std::mt19937_64 rng(0x5eed1234ULL);
  std::shuffle(assignment.begin(), assignment.end(), rng);

  Arm arm;
  for (size_t segment = 0; segment < p; ++segment) {
    arm.segments.push_back(std::make_unique<HNSWIndex>(options));
  }
  for (size_t i = 0; i < data.size(); ++i) {
    if (assignment[i] >= arm.segments.size()) {
      throw std::logic_error("segment assignment out of range");
    }
    MustOK(arm.segments[assignment[i]]->Insert(KeyFor(i), data[i]));
  }
  arm.node_count = data.size();
  return arm;
}

struct SweepPoint {
  size_t ef = 0;
  double recall = 0.0;
  double qps = 0.0;
};

// Recall and throughput of one arm across an ef sweep. The two arms are swept
// independently, so the iso-recall cost of partitioning can be read off the two
// curves instead of being fixed by a shared ef.
std::vector<SweepPoint> Sweep(const Arm& arm,
                              const std::vector<size_t>& ef_values,
                              const Corpus& queries,
                              const std::vector<std::set<std::string>>& truth) {
  std::vector<SweepPoint> curve;
  curve.reserve(ef_values.size());
  for (size_t ef : ef_values) {
    double recall = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < queries.size(); ++q) {
      recall += RecallOf(FanOutSearch(arm, queries[q], ef), truth[q]);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    curve.push_back({ef, recall / static_cast<double>(queries.size()),
                     static_cast<double>(queries.size()) / seconds});
  }
  return curve;
}

void PrintCurve(const char* distribution, const char* arm_label,
                const std::vector<SweepPoint>& curve) {
  for (const SweepPoint& point : curve) {
    std::printf("    %-18s %-12s ef=%-4zu recall@10=%.4f qps=%.1f\n",
                distribution, arm_label, point.ef, point.recall, point.qps);
  }
  std::fflush(stdout);
}

void Evaluate(const char* distribution,
              const Corpus& data,
              const Corpus& queries,
              const HNSWOptions& options,
              size_t p,
              bool verbose) {
  const auto truth = BruteTruth(data, queries);
  const Arm single = BuildSingle(data, options);
  const Arm partitioned = BuildPartitioned(data, options, p);
  if (verbose) {
    std::printf("    [%s] built single=%zu nodes, partitioned=%zu graphs x %zu nodes\n",
                distribution, single.node_count, partitioned.segments.size(),
                partitioned.node_count / partitioned.segments.size());
    std::fflush(stdout);
  }
  PrintCurve(distribution, "single_graph",
             Sweep(single, kSingleEfSweep, queries, truth));
  PrintCurve(distribution, "partitioned",
             Sweep(partitioned, kSegmentEfSweep, queries, truth));
}

int RunPartition(size_t n, size_t dim, size_t max_neighbors,
                 size_t ef_construction, size_t query_count, size_t p) {
  if (p < 1) throw std::invalid_argument("P must be at least 1");
  const HNSWOptions options = MakeOptions(dim, max_neighbors, ef_construction);

  std::printf("partition_probe backend=%s n=%zu dim=%zu M=%zu ef_c=%zu "
              "queries=%zu P=%zu\n",
              kv::VectorDistanceBackend(), n, dim, max_neighbors,
              ef_construction, query_count, p);
  std::fflush(stdout);

  Evaluate("iid_gaussian", IidGaussian(n, dim, 12345),
           IidGaussian(query_count, dim, 999), options, p, true);
  Evaluate("clustered_in_dist", Clustered(n, dim, 200, 12345, 23456),
           Clustered(query_count, dim, 200, 12345, 999), options, p, true);
  Evaluate("clustered_ood", Clustered(n, dim, 200, 12345, 23456),
           Clustered(query_count, dim, 200, 999, 1000), options, p, true);
  return 0;
}

// Three access patterns, because the FLOP argument in section 1.3 of the design
// document compares an achieved rate against a hardware peak. That comparison
// is only meaningful if the measured rate uses the same access pattern as the
// HNSW hot path.
//
//   stream : both operands advance (cold streaming, memory-bandwidth bound)
//   hnsw   : one hot query vector against a uniformly random target inside a
//            large contiguous array -- this is what SearchLayer actually does
//   hot    : one hot query vector against a 64-vector working set that stays in
//            cache -- this is the compute ceiling of the kernel
int RunKernel(size_t dim, size_t iterations, std::string_view pattern) {
  const bool hot = pattern == "hot";
  const size_t corpus_size = hot ? 64 : 20000;
  const Corpus data = IidGaussian(corpus_size, dim, 4242);

  std::vector<size_t> targets(iterations);
  if (pattern == "stream") {
    for (size_t i = 0; i < iterations; ++i) {
      targets[i] = i % corpus_size;
    }
  } else if (hot) {
    for (size_t i = 0; i < iterations; ++i) {
      targets[i] = i % corpus_size;
    }
  } else {
    std::mt19937_64 rng(31337);
    std::uniform_int_distribution<size_t> pick(0, corpus_size - 1);
    for (size_t& target : targets) {
      target = pick(rng);
    }
  }
  const std::vector<float>& query = data[0];

  const auto start = std::chrono::steady_clock::now();
  double accumulator = 0.0;
  for (size_t i = 0; i < iterations; ++i) {
    const size_t target = targets[i];
    // stream: both operands advance through the array.
    // hnsw / hot: one operand is the hot query vector, the other is the target.
    const float* lhs = pattern == "stream" ? data[target].data() : query.data();
    const float* rhs =
        pattern == "stream" ? data[(target + 1) % corpus_size].data()
                            : data[target].data();
    accumulator += kv::ComputeVectorDistanceUnchecked(lhs, rhs, dim,
                                                      VectorDistanceMetric::kL2);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  const double flops = 2.0 * static_cast<double>(dim) *
                       static_cast<double>(iterations);
  std::printf("kernel backend=%s pattern=%-7s dim=%zu iterations=%zu | %.2fs "
              "-> %.2f GFLOP/s (%.1f ns/distance)\n",
              kv::VectorDistanceBackend(), std::string(pattern).c_str(), dim,
              iterations, seconds, flops / seconds / 1e9,
              seconds * 1e9 / static_cast<double>(iterations));
  std::printf("kernel_sink=%.6f\n", accumulator);
  return 0;
}

// Measures the real distance-computation count per insert, replacing the
// modelled `ef_c * M` figure in section 1.3 of the design document. Requires a
// build configured with -DKV_HNSW_DISTANCE_COUNTER=ON; without it every count
// reads zero and the mode says so.
int RunDistance(size_t n, size_t dim, size_t max_neighbors,
                size_t ef_construction) {
  const Corpus data = IidGaussian(n, dim, 7);
  const HNSWOptions options = MakeOptions(dim, max_neighbors, ef_construction);
  HNSWIndex index(options);

  std::vector<std::uint64_t> per_insert(n, 0);
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < n; ++i) {
    kv::HNSWResetDistanceCallCount();
    MustOK(index.Insert(KeyFor(i), data[i]));
    per_insert[i] = kv::HNSWDistanceCallCount();
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  std::uint64_t total = 0;
  for (std::uint64_t count : per_insert) {
    total += count;
  }
  if (total == 0) {
    std::printf("distance_counter=DISABLED (rebuild with "
                "-DKV_HNSW_DISTANCE_COUNTER=ON)\n");
    return 1;
  }

  // The first inserts run on a nearly empty graph and cost almost nothing, so
  // the steady-state figure is taken over the second half only.
  const size_t half = n / 2;
  std::uint64_t steady_total = 0;
  for (size_t i = half; i < n; ++i) {
    steady_total += per_insert[i];
  }
  std::vector<std::uint64_t> sorted(per_insert.begin() + half, per_insert.end());
  std::sort(sorted.begin(), sorted.end());

  const double mean = static_cast<double>(total) / static_cast<double>(n);
  const double steady_mean =
      static_cast<double>(steady_total) / static_cast<double>(n - half);
  const double flops = 2.0 * static_cast<double>(dim) * static_cast<double>(total);

  std::printf("distance_probe backend=%s n=%zu dim=%zu M=%zu ef_c=%zu\n",
              kv::VectorDistanceBackend(), n, dim, max_neighbors,
              ef_construction);
  std::printf("    build_seconds=%.3f inserts_per_second=%.1f\n", seconds,
              static_cast<double>(n) / seconds);
  std::printf("    distance_calls_per_insert: all=%.1f steady_state(half)=%.1f "
              "p50=%llu p90=%llu max=%llu min=%llu\n",
              mean, steady_mean,
              static_cast<unsigned long long>(sorted[sorted.size() / 2]),
              static_cast<unsigned long long>(sorted[sorted.size() * 9 / 10]),
              static_cast<unsigned long long>(sorted.back()),
              static_cast<unsigned long long>(sorted.front()));
  std::printf("    achieved=%.2f GFLOP/s (total_calls=%llu, 2*dim per call)\n",
              flops / seconds / 1e9, static_cast<unsigned long long>(total));
  std::printf("    modelled_ef_c_times_M=%zu\n",
              ef_construction * max_neighbors);
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <partition|kernel|distance> [args]\n"
                 "  partition [n] [dim] [M] [ef_c] [queries] [P]\n"
                 "  kernel    [dim] [iterations] [stream|hnsw|hot]\n"
                 "  distance  [n] [dim] [M] [ef_c]   (needs "
                 "-DKV_HNSW_DISTANCE_COUNTER=ON)\n",
                 argv[0]);
    return 2;
  }
  const std::string_view mode = argv[1];
  const auto parse = [&](int position, size_t fallback) {
    if (argc <= position) return fallback;
    const std::string value(argv[position]);
    if (value.empty() ||
        value.find_first_not_of("0123456789") != std::string::npos) {
      throw std::invalid_argument("expected a nonnegative integer");
    }
    return static_cast<size_t>(std::stoull(value));
  };

  if (mode == "kernel") {
    const size_t dim = parse(2, 768);
    const size_t iterations = parse(3, 200000);
    std::string pattern = argc > 4 ? std::string(argv[4]) : "hnsw";
    if (pattern != "stream" && pattern != "hnsw" && pattern != "hot") {
      throw std::invalid_argument("kernel pattern must be stream|hnsw|hot");
    }
    if (dim == 0 || iterations == 0) {
      throw std::invalid_argument("dim and iterations must be positive");
    }
    return RunKernel(dim, iterations, pattern);
  }
  if (mode == "distance") {
    const size_t n = parse(2, 20000);
    const size_t dim = parse(3, 768);
    const size_t max_neighbors = parse(4, 16);
    const size_t ef_construction = parse(5, 200);
    if (argc > 6 || n < kTopK || dim == 0 || max_neighbors < 2 ||
        ef_construction == 0) {
      throw std::invalid_argument(
          "n>=10, dim>0, M>=2, ef_construction>0 required");
    }
    return RunDistance(n, dim, max_neighbors, ef_construction);
  }
  if (mode == "partition") {
    const size_t n = parse(2, 20000);
    const size_t dim = parse(3, 768);
    const size_t max_neighbors = parse(4, 16);
    const size_t ef_construction = parse(5, 200);
    const size_t query_count = parse(6, 100);
    const size_t p = parse(7, 8);
    if (argc > 8 || n < kTopK || dim == 0 || max_neighbors < 2 ||
        ef_construction == 0 || query_count == 0) {
      throw std::invalid_argument(
          "n>=10, dim>0, M>=2, ef_construction>0, queries>0 required");
    }
    return RunPartition(n, dim, max_neighbors, ef_construction, query_count, p);
  }
  std::fprintf(stderr, "unknown mode: %s\n", std::string(mode).c_str());
  return 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 2;
}
