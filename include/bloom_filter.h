#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "status.h"

namespace kv {

class BloomFilter {
 public:
  static BloomFilter Build(const std::vector<std::string>& keys, size_t bits_per_key);
  static Status Decode(const std::string& data, BloomFilter* filter);

  bool MayContain(std::string_view key) const;
  std::string Encode() const;

 private:
  static std::uint32_t Hash(std::string_view key, std::uint32_t seed);

  std::vector<std::uint8_t> bits_;
  std::uint32_t bit_count_ = 0;
  std::uint32_t hash_count_ = 0;
};

}  // namespace kv
