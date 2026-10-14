#include "storage_util.h"

#include <fcntl.h>
#include <unistd.h>

namespace kv {

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
