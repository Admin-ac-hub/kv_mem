#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "status.h"
#include "types.h"

namespace kv {

void EncodeFixed32(std::uint32_t value, char* out);
void EncodeFixed64(std::uint64_t value, char* out);
std::uint32_t DecodeFixed32(const char* in);
std::uint64_t DecodeFixed64(const char* in);
std::uint32_t CRC32(std::string_view data);

std::string EncodeInternalKey(std::string_view user_key, SequenceNumber sequence);
bool DecodeInternalKey(std::string_view internal_key,
                       std::string* user_key,
                       SequenceNumber* sequence);

std::string SSTableFileName(std::uint64_t number);
std::string WALFileName(std::uint64_t number);
bool ParseSSTableFileName(const std::filesystem::path& path, std::uint64_t* number);
bool ParseWALFileName(const std::filesystem::path& path, std::uint64_t* number);

Status RemoveFileIfExists(const std::filesystem::path& path);
Status FsyncFile(const std::filesystem::path& path);
Status FsyncDirectory(const std::filesystem::path& path);

}  // namespace kv
