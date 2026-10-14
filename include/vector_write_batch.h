#pragma once

#include "write_batch.h"
#include "vector_value.h"

namespace kv {

// Values use the ordinary KV Put encoding, including newly written vectors.
class VectorWriteBatch {
 public:
  void Put(std::string key, std::string value);
  Status PutVector(std::string key, const std::vector<float>& vector,
                   std::string metadata = {});
  void Delete(std::string key);
  void Clear() { batch_.Clear(); }
  size_t Count() const { return batch_.Count(); }
  std::string Encode() const { return batch_.Encode(); }
  const WriteBatch& KVBatch() const { return batch_; }

 private:
  WriteBatch batch_;
};

}  // namespace kv
