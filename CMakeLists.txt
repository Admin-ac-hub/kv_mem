cmake_minimum_required(VERSION 3.16)
project(mini_lsm_kv VERSION 0.1 LANGUAGES CXX)

include(CTest)
option(KV_ENABLE_RPC "Build Linux miniRPC service and client" OFF)
if(KV_ENABLE_VECTOR_EXTENSION OR KV_ENABLE_RECALL_REGRESSION)
    message(FATAL_ERROR "The HNSW/VectorDB extension has been removed; use a fresh build directory")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(cmake/Sanitizers.cmake)
find_package(Threads REQUIRED)

function(kv_target target)
    target_include_directories(${target} PUBLIC include)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

# Basic encoding, checksums, fsync and Status; no DB or index dependency.
add_library(kv_common src/storage_util.cc src/status.cc)
kv_target(kv_common)
add_library(kv_core
    src/block_cache.cc src/bloom_filter.cc src/db.cc src/format.cc
    src/manifest.cpp src/memtable.cc src/sstable.cpp src/skiplist.cc
    src/wal.cc src/write_batch.cc)
kv_target(kv_core)
target_link_libraries(kv_core PUBLIC kv_common Threads::Threads)
add_library(kvdb ALIAS kv_core)

foreach(target kv_bench week5_bench)
    add_executable(${target} bench/${target}.cpp)
    kv_target(${target})
    target_link_libraries(${target} PRIVATE kv_core)
endforeach()
add_executable(group_commit_bench bench/group_commit_bench.cpp)
kv_target(group_commit_bench)
target_link_libraries(group_commit_bench PRIVATE kv_core)
add_executable(kv_stress stress/kv_stress.cpp)
kv_target(kv_stress)
target_link_libraries(kv_stress PRIVATE kv_core)

function(kv_test_target target library)
    add_executable(${target} test/${target}.cc)
    kv_target(${target})
    target_link_libraries(${target} PRIVATE ${library})
    target_compile_definitions(${target} PRIVATE
        KV_TEST_DB_ROOT="${CMAKE_CURRENT_BINARY_DIR}/test_dbs"
        KV_TEST_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
    if(NOT KV_SANITIZERS STREQUAL "")
        set_tests_properties(${target} PROPERTIES TIMEOUT 300)
    endif()
endfunction()

if(BUILD_TESTING)
    kv_test_target(kv_test kv_core)
    kv_test_target(core_consumer_test kv_core)
    kv_test_target(core_boundary_test kv_core)
    kv_test_target(write_group_test kv_core)
    kv_test_target(read_version_test kv_core)
    kv_test_target(skiplist_test kv_core)
    set_tests_properties(write_group_test read_version_test skiplist_test PROPERTIES LABELS "core;concurrency")
    set_tests_properties(kv_test core_consumer_test core_boundary_test PROPERTIES LABELS core)
    add_test(NAME week5_bench_sync_smoke
        COMMAND week5_bench --mode sync --operations 10000 --threads 2 --batch-size 8)
    set_tests_properties(week5_bench_sync_smoke PROPERTIES LABELS "core;benchmark" TIMEOUT 60)
    add_test(NAME kv_stress_fixed_seed COMMAND kv_stress
        --path "${CMAKE_CURRENT_BINARY_DIR}/stress_dbs/kv_stress_fixed_seed"
        --epochs 2 --threads 4 --ops-per-thread 250 --keys-per-thread 128
        --value-size 64 --delete-percent 25 --batch-size 4
        --reopen-every-epochs 1 --compact-every-epochs 1 --seed 20260712
        --memtable-limit 32 --l0-limit 2)
    set_tests_properties(kv_stress_fixed_seed PROPERTIES LABELS "core;stress" TIMEOUT 120)
endif()

if(KV_ENABLE_RPC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "KV_ENABLE_RPC requires Linux (miniRPC uses epoll/eventfd)")
    endif()
    add_subdirectory(rpc)
endif()
