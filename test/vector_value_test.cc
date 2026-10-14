#include <cmath>
#include <limits>
#include "vector_value.h"
#include "test_support.h"

namespace {
void TestVectorValueEncodingAndDistances() {
  const std::vector<float> vector = {1.5f, -2.0f, 3.25f};
  const std::string metadata = "{\"source\":\"unit-test\",\"rank\":7}";

  std::string encoded;
  MustOK(kv::EncodeVectorValue(vector, metadata, &encoded));
  CHECK(kv::IsEncodedVectorValue(encoded));

  kv::VectorRecord decoded;
  MustOK(kv::DecodeVectorValue(encoded, &decoded));
  CHECK(decoded.vector == vector);
  CHECK(decoded.metadata == metadata);

  std::string truncated = encoded.substr(0, encoded.size() - 1);
  CHECK(!kv::DecodeVectorValue(truncated, &decoded).ok());
  std::string trailing = encoded + "x";
  CHECK(!kv::DecodeVectorValue(trailing, &decoded).ok());
  CHECK(!kv::EncodeVectorValue({}, metadata, &encoded).ok());
  CHECK(!kv::EncodeVectorValue(
      {std::numeric_limits<float>::quiet_NaN()}, metadata, &encoded).ok());

  float distance = 0.0f;
  MustOK(kv::ComputeVectorDistance(
      {0.0f, 0.0f}, {3.0f, 4.0f}, kv::VectorDistanceMetric::kL2,
      &distance));
  CHECK(std::fabs(distance - 5.0f) < 1e-6f);
  MustOK(kv::ComputeVectorDistance(
      {1.0f, 2.0f}, {3.0f, 4.0f},
      kv::VectorDistanceMetric::kInnerProduct, &distance));
  CHECK(std::fabs(distance + 11.0f) < 1e-6f);
  MustOK(kv::ComputeVectorDistance(
      {1.0f, 0.0f}, {0.0f, 1.0f}, kv::VectorDistanceMetric::kCosine,
      &distance));
  CHECK(std::fabs(distance - 1.0f) < 1e-6f);
  CHECK(!kv::ComputeVectorDistance(
      {1.0f}, {1.0f, 2.0f}, kv::VectorDistanceMetric::kL2,
      &distance).ok());
  CHECK(!kv::ComputeVectorDistance(
      {0.0f, 0.0f}, {1.0f, 0.0f}, kv::VectorDistanceMetric::kCosine,
      &distance).ok());
}
}
int main() { TestVectorValueEncodingAndDistances(); }
