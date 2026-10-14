#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "storage_util.h"
#include "types.h"

namespace kv {

std::string EncodeInternalKey(std::string_view user_key, SequenceNumber sequence);
bool DecodeInternalKey(std::string_view internal_key,
                       std::string* user_key,
                       SequenceNumber* sequence);

std::string SSTableFileName(std::uint64_t number);
std::string WALFileName(std::uint64_t number);
bool ParseSSTableFileName(const std::filesystem::path& path, std::uint64_t* number);
bool ParseWALFileName(const std::filesystem::path& path, std::uint64_t* number);

Status RemoveFileIfExists(const std::filesystem::path& path);

}  // namespace kv
