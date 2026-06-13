#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "status.h"

namespace kv {

enum class CompressionType : std::uint8_t {
  kNone = 0,
  kFake = 1,
};

class CompressionCodec {
 public:
  virtual ~CompressionCodec() = default;
  virtual CompressionType Type() const = 0;
  virtual Status Compress(std::string_view input, std::string* output) const = 0;
  virtual Status Uncompress(std::string_view input, std::string* output) const = 0;
};

std::unique_ptr<CompressionCodec> NewCompressionCodec(CompressionType type);

}  // namespace kv
