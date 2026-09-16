#include "vector_value.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#if !defined(KV_DISABLE_SIMD) && (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#define KV_CAN_USE_AVX2_INTRINSICS 1
#define KV_AVX2_TARGET __attribute__((target("avx2")))
#else
#define KV_CAN_USE_AVX2_INTRINSICS 0
#endif

#if !defined(KV_DISABLE_SIMD) && defined(__aarch64__)
#include <arm_neon.h>
#define KV_CAN_USE_NEON_INTRINSICS 1
#else
#define KV_CAN_USE_NEON_INTRINSICS 0
#endif

#include "format.h"

namespace kv {

namespace {

constexpr char kVectorValueMagic[] = {'K', 'V', 'V', '1'};
constexpr std::uint8_t kFloat32Type = 1;
constexpr size_t kHeaderSize = sizeof(kVectorValueMagic) + 1 + 4;
constexpr size_t kMetadataLengthSize = 4;

void AppendFixed32(std::string* output, std::uint32_t value) {
  const size_t offset = output->size();
  output->resize(offset + 4);
  EncodeFixed32(value, output->data() + offset);
}

Status ValidateVector(const std::vector<float>& vector) {
  if (vector.empty()) {
    return Status::InvalidArgument("vector cannot be empty");
  }
  if (vector.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("vector dimension exceeds uint32 range");
  }
  for (float value : vector) {
    if (!std::isfinite(value)) {
      return Status::InvalidArgument("vector values must be finite");
    }
  }
  return Status::OK();
}

struct DistanceAccumulators {
  double dot = 0.0;
  double squared_l2 = 0.0;
  double lhs_norm = 0.0;
  double rhs_norm = 0.0;
};

DistanceAccumulators AccumulateScalar(const float* lhs,
                                      const float* rhs,
                                      size_t dimension,
                                      VectorDistanceMetric metric) {
  DistanceAccumulators values;
  for (size_t i = 0; i < dimension; ++i) {
    const double left = lhs[i];
    const double right = rhs[i];
    switch (metric) {
      case VectorDistanceMetric::kL2: {
        const double difference = left - right;
        values.squared_l2 += difference * difference;
        break;
      }
      case VectorDistanceMetric::kInnerProduct:
        values.dot += left * right;
        break;
      case VectorDistanceMetric::kCosine:
        values.dot += left * right;
        values.lhs_norm += left * left;
        values.rhs_norm += right * right;
        break;
    }
  }
  return values;
}

#if KV_CAN_USE_NEON_INTRINSICS
// AArch64 provides NEON as part of its base ISA. Widen before multiplying to
// retain the scalar path's range and avoid float accumulation error/overflow.
template <VectorDistanceMetric metric>
DistanceAccumulators AccumulateNEON(const float* lhs, const float* rhs,
                                    size_t dimension) {
  float64x2_t dot0 = vdupq_n_f64(0), dot1 = vdupq_n_f64(0);
  float64x2_t left0 = vdupq_n_f64(0), left1 = vdupq_n_f64(0);
  float64x2_t right0 = vdupq_n_f64(0), right1 = vdupq_n_f64(0);
  size_t i = 0;
  for (; i + 4 <= dimension; i += 4) {
    const float32x4_t l = vld1q_f32(lhs + i), r = vld1q_f32(rhs + i);
    const float64x2_t l0 = vcvt_f64_f32(vget_low_f32(l));
    const float64x2_t l1 = vcvt_f64_f32(vget_high_f32(l));
    const float64x2_t r0 = vcvt_f64_f32(vget_low_f32(r));
    const float64x2_t r1 = vcvt_f64_f32(vget_high_f32(r));
    if constexpr (metric == VectorDistanceMetric::kL2) {
      const auto d0 = vsubq_f64(l0, r0), d1 = vsubq_f64(l1, r1);
      dot0 = vaddq_f64(dot0, vmulq_f64(d0, d0));
      dot1 = vaddq_f64(dot1, vmulq_f64(d1, d1));
    } else {
      dot0 = vaddq_f64(dot0, vmulq_f64(l0, r0));
      dot1 = vaddq_f64(dot1, vmulq_f64(l1, r1));
      if constexpr (metric == VectorDistanceMetric::kCosine) {
        left0 = vaddq_f64(left0, vmulq_f64(l0, l0));
        left1 = vaddq_f64(left1, vmulq_f64(l1, l1));
        right0 = vaddq_f64(right0, vmulq_f64(r0, r0));
        right1 = vaddq_f64(right1, vmulq_f64(r1, r1));
      }
    }
  }
  DistanceAccumulators values = AccumulateScalar(lhs + i, rhs + i, dimension - i, metric);
  const double dot = vaddvq_f64(vaddq_f64(dot0, dot1));
  if constexpr (metric == VectorDistanceMetric::kL2) {
    values.squared_l2 += dot;
  } else {
    values.dot += dot;
    if constexpr (metric == VectorDistanceMetric::kCosine) {
      values.lhs_norm += vaddvq_f64(vaddq_f64(left0, left1));
      values.rhs_norm += vaddvq_f64(vaddq_f64(right0, right1));
    }
  }
  return values;
}
#endif

#if KV_CAN_USE_AVX2_INTRINSICS
KV_AVX2_TARGET DistanceAccumulators AccumulateAVX2(
    const float* lhs,
    const float* rhs,
    size_t dimension,
    VectorDistanceMetric metric) {
  DistanceAccumulators values;
  size_t i = 0;
  alignas(32) float lanes[8];
  for (; i + 8 <= dimension; i += 8) {
    const __m256 left = _mm256_loadu_ps(lhs + i);
    const __m256 right = _mm256_loadu_ps(rhs + i);
    __m256 value;
    switch (metric) {
      case VectorDistanceMetric::kL2: {
        const __m256 difference = _mm256_sub_ps(left, right);
        value = _mm256_mul_ps(difference, difference);
        _mm256_store_ps(lanes, value);
        for (float lane : lanes) {
          values.squared_l2 += static_cast<double>(lane);
        }
        break;
      }
      case VectorDistanceMetric::kInnerProduct:
        value = _mm256_mul_ps(left, right);
        _mm256_store_ps(lanes, value);
        for (float lane : lanes) {
          values.dot += static_cast<double>(lane);
        }
        break;
      case VectorDistanceMetric::kCosine:
        value = _mm256_mul_ps(left, right);
        _mm256_store_ps(lanes, value);
        for (float lane : lanes) {
          values.dot += static_cast<double>(lane);
        }
        value = _mm256_mul_ps(left, left);
        _mm256_store_ps(lanes, value);
        for (float lane : lanes) {
          values.lhs_norm += static_cast<double>(lane);
        }
        value = _mm256_mul_ps(right, right);
        _mm256_store_ps(lanes, value);
        for (float lane : lanes) {
          values.rhs_norm += static_cast<double>(lane);
        }
        break;
    }
  }
  const DistanceAccumulators tail =
      AccumulateScalar(lhs + i, rhs + i, dimension - i, metric);
  values.dot += tail.dot;
  values.squared_l2 += tail.squared_l2;
  values.lhs_norm += tail.lhs_norm;
  values.rhs_norm += tail.rhs_norm;
  return values;
}

bool RuntimeSupportsAVX2() {
  return __builtin_cpu_supports("avx2");
}
#else
bool RuntimeSupportsAVX2() {
  return false;
}
#endif

float FinalizeDistance(const DistanceAccumulators& values,
                       VectorDistanceMetric metric) {
  switch (metric) {
    case VectorDistanceMetric::kL2:
      return static_cast<float>(std::sqrt(values.squared_l2));
    case VectorDistanceMetric::kInnerProduct:
      return static_cast<float>(-values.dot);
    case VectorDistanceMetric::kCosine:
      return static_cast<float>(1.0 - std::clamp(
          values.dot / std::sqrt(values.lhs_norm * values.rhs_norm),
          -1.0, 1.0));
  }
  return std::numeric_limits<float>::quiet_NaN();
}

}  // namespace

Status EncodeVectorValue(const std::vector<float>& vector,
                         std::string_view metadata,
                         std::string* encoded) {
  if (encoded == nullptr) {
    return Status::InvalidArgument("encoded output cannot be null");
  }
  Status status = ValidateVector(vector);
  if (!status.ok()) {
    return status;
  }
  if (metadata.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("metadata size exceeds uint32 range");
  }

  encoded->clear();
  encoded->reserve(kHeaderSize + vector.size() * sizeof(float) +
                   kMetadataLengthSize + metadata.size());
  encoded->append(kVectorValueMagic, sizeof(kVectorValueMagic));
  encoded->push_back(static_cast<char>(kFloat32Type));
  AppendFixed32(encoded, static_cast<std::uint32_t>(vector.size()));
  for (float value : vector) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float32 encoding requires 32-bit float");
    std::memcpy(&bits, &value, sizeof(bits));
    AppendFixed32(encoded, bits);
  }
  AppendFixed32(encoded, static_cast<std::uint32_t>(metadata.size()));
  if (!metadata.empty()) {
    encoded->append(metadata.data(), metadata.size());
  }
  return Status::OK();
}

bool IsEncodedVectorValue(std::string_view encoded) {
  return encoded.size() >= sizeof(kVectorValueMagic) &&
         std::memcmp(encoded.data(), kVectorValueMagic,
                     sizeof(kVectorValueMagic)) == 0;
}

Status DecodeVectorValue(std::string_view encoded, VectorRecord* record) {
  if (record == nullptr) {
    return Status::InvalidArgument("vector record output cannot be null");
  }
  if (!IsEncodedVectorValue(encoded)) {
    return Status::InvalidArgument("value is not an encoded vector");
  }
  if (encoded.size() < kHeaderSize + kMetadataLengthSize) {
    return Status::Corruption("encoded vector header is truncated");
  }

  size_t offset = sizeof(kVectorValueMagic);
  const auto type = static_cast<std::uint8_t>(encoded[offset++]);
  if (type != kFloat32Type) {
    return Status::Corruption("unsupported encoded vector type");
  }

  const std::uint32_t dimension = DecodeFixed32(encoded.data() + offset);
  offset += 4;
  if (dimension == 0) {
    return Status::Corruption("encoded vector has zero dimension");
  }
  const size_t vector_bytes = static_cast<size_t>(dimension) * sizeof(float);
  if (vector_bytes > encoded.size() - offset ||
      encoded.size() - offset - vector_bytes < kMetadataLengthSize) {
    return Status::Corruption("encoded vector payload is truncated");
  }

  VectorRecord decoded;
  decoded.vector.resize(dimension);
  for (std::uint32_t i = 0; i < dimension; ++i) {
    const std::uint32_t bits = DecodeFixed32(encoded.data() + offset);
    std::memcpy(&decoded.vector[i], &bits, sizeof(bits));
    if (!std::isfinite(decoded.vector[i])) {
      return Status::Corruption("encoded vector contains a non-finite value");
    }
    offset += sizeof(float);
  }

  const std::uint32_t metadata_size = DecodeFixed32(encoded.data() + offset);
  offset += kMetadataLengthSize;
  if (metadata_size != encoded.size() - offset) {
    return Status::Corruption("encoded vector metadata length does not match payload");
  }
  decoded.metadata.assign(encoded.data() + offset, metadata_size);
  *record = std::move(decoded);
  return Status::OK();
}

Status ComputeVectorDistance(const std::vector<float>& lhs,
                             const std::vector<float>& rhs,
                             VectorDistanceMetric metric,
                             float* distance) {
  if (distance == nullptr) {
    return Status::InvalidArgument("distance output cannot be null");
  }
  Status status = ValidateVector(lhs);
  if (!status.ok()) {
    return status;
  }
  status = ValidateVector(rhs);
  if (!status.ok()) {
    return status;
  }
  if (lhs.size() != rhs.size()) {
    return Status::InvalidArgument("vector dimensions do not match");
  }

  if (metric != VectorDistanceMetric::kL2 &&
      metric != VectorDistanceMetric::kInnerProduct &&
      metric != VectorDistanceMetric::kCosine) {
    return Status::InvalidArgument("unknown vector distance metric");
  }
  if (metric == VectorDistanceMetric::kCosine) {
    const auto has_nonzero = [](const std::vector<float>& vector) {
      return std::any_of(vector.begin(), vector.end(),
                         [](float value) { return value != 0.0f; });
    };
    if (!has_nonzero(lhs) || !has_nonzero(rhs)) {
      return Status::InvalidArgument("cosine distance is undefined for a zero vector");
    }
  }

  const float result = ComputeVectorDistanceUnchecked(
      lhs.data(), rhs.data(), lhs.size(), metric);

  if (!std::isfinite(result) ||
      result > std::numeric_limits<float>::max() ||
      result < -std::numeric_limits<float>::max()) {
    return Status::InvalidArgument("vector distance is outside float range");
  }
  *distance = result;
  return Status::OK();
}

float ComputeVectorDistanceUnchecked(const float* lhs,
                                     const float* rhs,
                                     size_t dimension,
                                     VectorDistanceMetric metric) {
#if KV_CAN_USE_NEON_INTRINSICS
  switch (metric) {
    case VectorDistanceMetric::kL2:
      return FinalizeDistance(AccumulateNEON<VectorDistanceMetric::kL2>(
          lhs, rhs, dimension), metric);
    case VectorDistanceMetric::kInnerProduct:
      return FinalizeDistance(AccumulateNEON<VectorDistanceMetric::kInnerProduct>(
          lhs, rhs, dimension), metric);
    case VectorDistanceMetric::kCosine:
      return FinalizeDistance(AccumulateNEON<VectorDistanceMetric::kCosine>(
          lhs, rhs, dimension), metric);
  }
#endif
#if KV_CAN_USE_AVX2_INTRINSICS
  if (RuntimeSupportsAVX2()) {
    return FinalizeDistance(AccumulateAVX2(lhs, rhs, dimension, metric),
                            metric);
  }
#endif
  return FinalizeDistance(AccumulateScalar(lhs, rhs, dimension, metric),
                          metric);
}

bool VectorDistanceUsesAVX2() {
  return RuntimeSupportsAVX2();
}

const char* VectorDistanceBackend() {
#if KV_CAN_USE_NEON_INTRINSICS
  return "neon";
#else
  return RuntimeSupportsAVX2() ? "avx2" : "scalar";
#endif
}

}  // namespace kv
