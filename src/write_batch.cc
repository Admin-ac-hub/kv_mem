#include "write_batch.h"

#include <utility>

#include "format.h"

namespace kv {

namespace {

constexpr size_t kOperationHeaderSize = 9;

void AppendFixed32(std::string* out, std::uint32_t value) {
  const size_t old_size = out->size();
  out->resize(old_size + 4);
  EncodeFixed32(value, out->data() + old_size);
}

}  // namespace

void WriteBatch::Put(std::string key, std::string value) {
  operations_.push_back(
      WriteBatchOperation{WriteBatchOpType::kPut, std::move(key), std::move(value)});
}

void WriteBatch::Delete(std::string key) {
  operations_.push_back(WriteBatchOperation{WriteBatchOpType::kDelete, std::move(key), ""});
}

void WriteBatch::Clear() {
  operations_.clear();
}

size_t WriteBatch::Count() const {
  return operations_.size();
}

std::string WriteBatch::Encode() const {
  std::string encoded;
  for (const auto& operation : operations_) {
    encoded.push_back(static_cast<char>(operation.type));
    AppendFixed32(&encoded, static_cast<std::uint32_t>(operation.key.size()));
    AppendFixed32(&encoded, static_cast<std::uint32_t>(operation.value.size()));
    encoded.append(operation.key);
    encoded.append(operation.value);
  }
  return encoded;
}

Status WriteBatch::Decode(std::string_view encoded) {
  const char* ptr = encoded.data();
  const char* end = ptr + encoded.size();
  std::vector<WriteBatchOperation> decoded;

  while (ptr != end) {
    if (end - ptr < static_cast<std::ptrdiff_t>(kOperationHeaderSize)) {
      return Status::Corruption("invalid WriteBatch operation header");
    }

    const auto type = static_cast<WriteBatchOpType>(static_cast<std::uint8_t>(ptr[0]));
    if (type != WriteBatchOpType::kPut && type != WriteBatchOpType::kDelete) {
      return Status::Corruption("unknown WriteBatch operation type");
    }

    const std::uint32_t key_size = DecodeFixed32(ptr + 1);
    const std::uint32_t value_size = DecodeFixed32(ptr + 5);
    ptr += kOperationHeaderSize;
    if (end - ptr < static_cast<std::ptrdiff_t>(key_size + value_size)) {
      return Status::Corruption("invalid WriteBatch operation payload");
    }

    WriteBatchOperation operation;
    operation.type = type;
    operation.key.assign(ptr, key_size);
    ptr += key_size;
    operation.value.assign(ptr, value_size);
    ptr += value_size;

    if (operation.type == WriteBatchOpType::kDelete && !operation.value.empty()) {
      return Status::Corruption("delete operation carries a value");
    }
    decoded.push_back(std::move(operation));
  }

  operations_ = std::move(decoded);
  return Status::OK();
}

const std::vector<WriteBatchOperation>& WriteBatch::Operations() const {
  return operations_;
}

}  // namespace kv
