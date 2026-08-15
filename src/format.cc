#include "format.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace kv {

namespace {

bool ParseNumberedName(const std::filesystem::path& path,
                       const std::string& prefix,
                       const std::string& suffix,
                       std::uint64_t* number) {
  const std::string name = path.filename().string();
  if (name.size() != prefix.size() + 6 + suffix.size() ||
      name.rfind(prefix, 0) != 0 ||
      name.substr(prefix.size() + 6) != suffix) {
    return false;
  }

  const std::string digits = name.substr(prefix.size(), 6);
  if (!std::all_of(digits.begin(), digits.end(),
                   [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
    return false;
  }

  *number = static_cast<std::uint64_t>(std::stoull(digits));
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

void EncodeFixed32(std::uint32_t value, char* out) {
  out[0] = static_cast<char>(value & 0xff);
  out[1] = static_cast<char>((value >> 8) & 0xff);
  out[2] = static_cast<char>((value >> 16) & 0xff);
  out[3] = static_cast<char>((value >> 24) & 0xff);
}

void EncodeFixed64(std::uint64_t value, char* out) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<char>((value >> (8 * i)) & 0xff);
  }
}

std::uint32_t DecodeFixed32(const char* in) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(in[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(in[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(in[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(in[3])) << 24);
}

std::uint64_t DecodeFixed64(const char* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[i])) << (8 * i);
  }
  return value;
}

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

std::uint32_t CRC32(std::string_view data) {
  std::uint32_t crc = 0xffffffffu;
  for (unsigned char byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return ~crc;
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

Status FsyncFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("failed to open file for fsync: " + path.string());
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return Status::IOError("failed to fsync file: " + path.string());
  }
  if (::close(fd) != 0) {
    return Status::IOError("failed to close fsynced file: " + path.string());
  }
  return Status::OK();
}

Status FsyncDirectory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("failed to open directory for fsync: " + path.string());
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return Status::IOError("failed to fsync directory: " + path.string());
  }
  if (::close(fd) != 0) {
    return Status::IOError("failed to close fsynced directory: " + path.string());
  }
  return Status::OK();
}

}  // namespace kv
