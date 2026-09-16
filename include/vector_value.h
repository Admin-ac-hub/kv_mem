#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "status.h"

namespace kv {

enum class VectorDistanceMetric {
  kL2,
  kInnerProduct,
  kCosine,
};

struct VectorRecord {
  std::string key;
  std::vector<float> vector;
  std::string metadata;
};

struct VectorResult {
  std::string key;
  float distance = 0.0f;
  std::string metadata;
};

// Encoded values carry a magic prefix so they can coexist with ordinary KV
// values in the same LSM tree.
Status EncodeVectorValue(const std::vector<float>& vector,
                         std::string_view metadata,
                         std::string* encoded);
Status DecodeVectorValue(std::string_view encoded, VectorRecord* record);
bool IsEncodedVectorValue(std::string_view encoded);

// All metrics use smaller-is-better distance semantics. Inner product is
// returned as the negated dot product; cosine distance is 1 - similarity.
Status ComputeVectorDistance(const std::vector<float>& lhs,
                             const std::vector<float>& rhs,
                             VectorDistanceMetric metric,
                             float* distance);

// This is the validation-free form used by the HNSW hot path after dimensions
// and vector contents have already been checked at insertion/query boundaries.
float ComputeVectorDistanceUnchecked(const float* lhs,
                                     const float* rhs,
                                     size_t dimension,
                                     VectorDistanceMetric metric);

// True only when the process can use the optional AVX2 implementation.
bool VectorDistanceUsesAVX2();
// "avx2", "neon", or "scalar" for reproducible benchmark reports.
const char* VectorDistanceBackend();

}  // namespace kv
