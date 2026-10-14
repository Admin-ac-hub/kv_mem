#include "vector_write_batch.h"

#include <utility>

namespace kv {

void VectorWriteBatch::Put(std::string key, std::string value) {
  batch_.Put(std::move(key), std::move(value));
}

Status VectorWriteBatch::PutVector(std::string key,
                                   const std::vector<float>& vector,
                                   std::string metadata) {
  std::string encoded;
  Status status = EncodeVectorValue(vector, metadata, &encoded);
  if (status.ok()) {
    batch_.Put(std::move(key), std::move(encoded));
  }
  return status;
}

void VectorWriteBatch::Delete(std::string key) {
  batch_.Delete(std::move(key));
}

}  // namespace kv
