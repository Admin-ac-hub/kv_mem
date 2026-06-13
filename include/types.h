#pragma once

#include <cstdint>
#include <string>

namespace kv {

using SequenceNumber = std::uint64_t;

struct VersionedEntry {
  std::string key;
  SequenceNumber sequence = 0;
  std::string value;
  bool deleted = false;
};

}  // namespace kv
