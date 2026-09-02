#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "status.h"
#include "vector_value.h"

namespace kv {

enum class WriteBatchOpType : std::uint8_t {
  kPut = 1,
  kDelete = 2,
  kPutVector = 3,
};

struct WriteBatchOperation {
  WriteBatchOpType type = WriteBatchOpType::kPut;
  std::string key;
  std::string value;
};

class WriteBatch {
 public:
  void Put(std::string key, std::string value);
  Status PutVector(std::string key,
                   const std::vector<float>& vector,
                   std::string metadata = {});
  void Delete(std::string key);
  void Clear();
  size_t Count() const;

  std::string Encode() const;
  Status Decode(std::string_view encoded);

  const std::vector<WriteBatchOperation>& Operations() const;

 private:
  std::vector<WriteBatchOperation> operations_;
};

}  // namespace kv
