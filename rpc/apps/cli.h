#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

inline bool ParsePort(std::string_view text, std::uint16_t* port) {
  unsigned value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || value > 65535) return false;
  *port = static_cast<std::uint16_t>(value);
  return true;
}
