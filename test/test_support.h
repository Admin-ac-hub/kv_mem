#pragma once
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "format.h"

namespace {

constexpr size_t kFlushLimit = 1024;

[[noreturn]] inline void CheckFailed(const char* expression, const char* file, int line) {
  std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
  std::abort();
}

#define CHECK(expression)                     \
  do {                                        \
    if (!(expression)) {                      \
      CheckFailed(#expression, __FILE__, __LINE__); \
    }                                         \
  } while (false)

#ifndef KV_TEST_DB_ROOT
#define KV_TEST_DB_ROOT "test/test_dbs"
#endif

inline std::filesystem::path TestDBPath(const std::string& name) {
  std::filesystem::path path = std::filesystem::path(KV_TEST_DB_ROOT) / name;
  std::filesystem::create_directories(path.parent_path());
  return path;
}

inline void MustOK(const kv::Status& status) {
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    std::abort();
  }
}

inline std::string Key(size_t index) {
  return "k" + std::to_string(index);
}

template <class Database>
void FillToFlush(Database* db, const std::string& prefix, const std::string& value) {
  for (size_t i = 0; i < kFlushLimit; ++i) {
    MustOK(db->Put(prefix + Key(i), value + std::to_string(i)));
  }
}

template <class Database>
bool WaitForSSTableCount(Database* db, size_t count) {
  for (int i = 0; i < 200; ++i) {
    if (db->Stats().sstable_count >= count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

template <class Database>
bool WaitForCompactionCount(Database* db, std::uint64_t count) {
  for (int i = 0; i < 200; ++i) {
    if (db->Stats().compaction_count >= count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

template <class Database>
bool WaitForLevel2SSTable(Database* db) {
  for (int i = 0; i < 200; ++i) {
    if (db->Stats().level2_sstable_count > 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

inline std::vector<std::filesystem::path> ListSSTables(const std::filesystem::path& db_path) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
    std::uint64_t number = 0;
    if (entry.is_regular_file() && kv::ParseSSTableFileName(entry.path(), &number)) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

inline std::vector<std::filesystem::path> ListHNSWIndexes(
    const std::filesystem::path& db_path) {
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::exists(db_path)) {
    return files;
  }
  for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
    if (entry.is_regular_file() && entry.path().extension() == ".hnsw") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace
