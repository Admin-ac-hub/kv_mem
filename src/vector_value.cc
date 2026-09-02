#include "vector_value.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

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

  double dot = 0.0;
  double squared_l2 = 0.0;
  double lhs_norm = 0.0;
  double rhs_norm = 0.0;
  for (size_t i = 0; i < lhs.size(); ++i) {
    const double left = lhs[i];
    const double right = rhs[i];
    const double difference = left - right;
    dot += left * right;
    squared_l2 += difference * difference;
    lhs_norm += left * left;
    rhs_norm += right * right;
  }

  double result = 0.0;
  switch (metric) {
    case VectorDistanceMetric::kL2:
      result = std::sqrt(squared_l2);
      break;
    case VectorDistanceMetric::kInnerProduct:
      result = -dot;
      break;
    case VectorDistanceMetric::kCosine:
      if (lhs_norm == 0.0 || rhs_norm == 0.0) {
        return Status::InvalidArgument("cosine distance is undefined for a zero vector");
      }
      result = 1.0 - std::clamp(dot / std::sqrt(lhs_norm * rhs_norm),
                                -1.0, 1.0);
      break;
    default:
      return Status::InvalidArgument("unknown vector distance metric");
  }

  if (!std::isfinite(result) ||
      result > std::numeric_limits<float>::max() ||
      result < -std::numeric_limits<float>::max()) {
    return Status::InvalidArgument("vector distance is outside float range");
  }
  *distance = static_cast<float>(result);
  return Status::OK();
}

}  // namespace kv
