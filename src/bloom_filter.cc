#include "bloom_filter.h"

#include <algorithm>
#include <array>

#include "format.h"

namespace kv {

BloomFilter BloomFilter::Build(const std::vector<std::string>& keys, size_t bits_per_key) {
  BloomFilter filter;
  filter.bit_count_ = static_cast<std::uint32_t>(
      std::max<size_t>(64, keys.size() * std::max<size_t>(1, bits_per_key)));
  filter.bits_.assign((filter.bit_count_ + 7) / 8, 0);
  filter.hash_count_ = static_cast<std::uint32_t>(
      std::max<size_t>(1, std::min<size_t>(30, static_cast<size_t>(bits_per_key * 0.69))));

  for (const auto& key : keys) {
    const std::uint32_t h1 = Hash(key, 0x9747b28c);
    const std::uint32_t h2 = Hash(key, 0x5bd1e995);
    for (std::uint32_t i = 0; i < filter.hash_count_; ++i) {
      const std::uint32_t bit = (h1 + i * h2) % filter.bit_count_;
      filter.bits_[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
    }
  }

  return filter;
}

Status BloomFilter::Decode(const std::string& data, BloomFilter* filter) {
  if (data.size() < 8) {
    return Status::Corruption("invalid bloom filter");
  }
  filter->bit_count_ = DecodeFixed32(data.data());
  filter->hash_count_ = DecodeFixed32(data.data() + 4);
  const size_t expected = static_cast<size_t>((filter->bit_count_ + 7) / 8);
  if (data.size() != expected + 8 || filter->hash_count_ == 0) {
    return Status::Corruption("invalid bloom filter payload");
  }
  filter->bits_.assign(data.begin() + 8, data.end());
  return Status::OK();
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (bit_count_ == 0 || hash_count_ == 0) {
    return true;
  }
  const std::uint32_t h1 = Hash(key, 0x9747b28c);
  const std::uint32_t h2 = Hash(key, 0x5bd1e995);
  for (std::uint32_t i = 0; i < hash_count_; ++i) {
    const std::uint32_t bit = (h1 + i * h2) % bit_count_;
    if ((bits_[bit / 8] & static_cast<std::uint8_t>(1u << (bit % 8))) == 0) {
      return false;
    }
  }
  return true;
}

std::string BloomFilter::Encode() const {
  std::string data(8, '\0');
  EncodeFixed32(bit_count_, data.data());
  EncodeFixed32(hash_count_, data.data() + 4);
  data.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
  return data;
}

std::uint32_t BloomFilter::Hash(std::string_view key, std::uint32_t seed) {
  std::uint32_t hash = seed ^ static_cast<std::uint32_t>(key.size());
  for (unsigned char ch : key) {
    hash ^= ch;
    hash *= 0x5bd1e995;
    hash ^= hash >> 15;
  }
  return hash;
}

}  // namespace kv
