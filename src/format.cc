#include "format.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace kv {

namespace {

bool ParseNumberedName(const std::filesystem::path& path,
                       const std::string& prefix,
                       const std::string& suffix,
                       std::uint64_t* number) {
  const std::string name = path.filename().string();
  if (name.size() < prefix.size() + 6 + suffix.size() ||
      name.rfind(prefix, 0) != 0 ||
      name.substr(name.size() - suffix.size()) != suffix) {
    return false;
  }

  const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (!std::all_of(digits.begin(), digits.end(),
                   [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
    return false;
  }

  std::uint64_t parsed = 0;
  for (char digit : digits) {
    const auto value = static_cast<std::uint64_t>(digit - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - value) / 10) return false;
    parsed = parsed * 10 + value;
  }
  if (parsed == 0) return false;
  *number = parsed;
  return true;
}

std::string NumberedName(const std::string& prefix,
                         std::uint64_t number,
                         const std::string& suffix) {
  std::ostringstream out;
  out << prefix << std::setw(6) << std::setfill('0') << number << suffix;
  return out.str();
}

}  // namespace

std::string EncodeInternalKey(std::string_view user_key, SequenceNumber sequence) {
  const SequenceNumber inverted = std::numeric_limits<SequenceNumber>::max() - sequence;
  std::string encoded(user_key);
  encoded.push_back('\0');
  for (int i = 7; i >= 0; --i) {
    encoded.push_back(static_cast<char>((inverted >> (i * 8)) & 0xff));
  }
  return encoded;
}

bool DecodeInternalKey(std::string_view internal_key,
                       std::string* user_key,
                       SequenceNumber* sequence) {
  constexpr size_t kTrailerSize = 1 + sizeof(SequenceNumber);
  if (internal_key.size() < kTrailerSize ||
      internal_key[internal_key.size() - kTrailerSize] != '\0') {
    return false;
  }

  user_key->assign(internal_key.data(), internal_key.size() - kTrailerSize);
  SequenceNumber inverted = 0;
  const char* ptr = internal_key.data() + internal_key.size() - sizeof(SequenceNumber);
  for (int i = 0; i < 8; ++i) {
    inverted = (inverted << 8) |
               static_cast<SequenceNumber>(static_cast<unsigned char>(ptr[i]));
  }
  *sequence = std::numeric_limits<SequenceNumber>::max() - inverted;
  return true;
}

std::string SSTableFileName(std::uint64_t number) {
  return NumberedName("sst_", number, ".data");
}

std::string WALFileName(std::uint64_t number) {
  return NumberedName("wal_", number, ".log");
}

bool ParseSSTableFileName(const std::filesystem::path& path, std::uint64_t* number) {
  return ParseNumberedName(path, "sst_", ".data", number);
}

bool ParseWALFileName(const std::filesystem::path& path, std::uint64_t* number) {
  return ParseNumberedName(path, "wal_", ".log", number);
}

Status RemoveFileIfExists(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Status::IOError("failed to remove file: " + path.string() + ": " + ec.message());
  }
  return Status::OK();
}

}  // namespace kv
