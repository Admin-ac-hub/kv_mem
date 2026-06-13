cmake_minimum_required(VERSION 3.16)
project(kv_mem VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(kvdb
    src/block_cache.cc
    src/bloom_filter.cc
    src/compression.cc
    src/db.cc
    src/format.cc
    src/manifest.cpp
    src/memtable.cc
    src/sstable.cpp
    src/skiplist.cc
    src/wal.cc
    src/write_batch.cc
)

target_include_directories(kvdb PUBLIC include)
target_compile_options(kvdb PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_demo src/main.cc)
target_link_libraries(kv_demo PRIVATE kvdb)
target_compile_options(kv_demo PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_test test/kv_test.cc)
target_link_libraries(kv_test PRIVATE kvdb)
target_compile_definitions(kv_test PRIVATE KV_TEST_DB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/test/test_dbs")
target_compile_options(kv_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_bench bench/kv_bench.cpp)
target_link_libraries(kv_bench PRIVATE kvdb)
target_compile_options(kv_bench PRIVATE -Wall -Wextra -Wpedantic)
