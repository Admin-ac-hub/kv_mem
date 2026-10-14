cmake_minimum_required(VERSION 3.16)
project(mini_lsm_kv VERSION 0.1 LANGUAGES CXX)

include(CTest)
option(KV_ENABLE_VECTOR_EXTENSION "Build VectorDB and HNSW extension" OFF)
option(KV_ENABLE_RECALL_REGRESSION "Run 20k-vector recall regressions (slow)" OFF)
option(KV_ENABLE_SIMD "Enable supported vector-distance SIMD backends" ON)
option(KV_HNSW_DISTANCE_COUNTER "Count HNSW distance computations" OFF)
if(KV_ENABLE_RECALL_REGRESSION AND NOT KV_ENABLE_VECTOR_EXTENSION)
    message(FATAL_ERROR "KV_ENABLE_RECALL_REGRESSION requires KV_ENABLE_VECTOR_EXTENSION=ON")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(cmake/Sanitizers.cmake)
find_package(Threads REQUIRED)

function(kv_target target)
    target_include_directories(${target} PUBLIC include)
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

if(KV_ENABLE_VECTOR_EXTENSION)
    add_library(vector_value src/vector_value.cc)
    kv_target(vector_value)
    target_link_libraries(vector_value PUBLIC kv_common)
    if(NOT KV_ENABLE_SIMD)
        target_compile_definitions(vector_value PRIVATE KV_DISABLE_SIMD=1)
    endif()
    add_library(hnsw src/hnsw_graph.cc src/hnsw_index.cc)
    kv_target(hnsw)
    target_link_libraries(hnsw PUBLIC vector_value Threads::Threads)
    if(KV_HNSW_DISTANCE_COUNTER)
        target_compile_definitions(hnsw PRIVATE KV_HNSW_DISTANCE_COUNTER=1)
    endif()
    add_library(vector_db src/vector_db.cc src/vector_write_batch.cc)
    kv_target(vector_db)
    target_link_libraries(vector_db PUBLIC kv_core hnsw)

    add_executable(vector_bench bench/vector_bench.cpp)
    kv_target(vector_bench)
    target_link_libraries(vector_bench PRIVATE vector_db)
    foreach(target hnsw_recall_probe hnsw_partition_probe)
        add_executable(${target} tools/${target}.cpp)
        kv_target(${target})
        target_link_libraries(${target} PRIVATE hnsw)
    endforeach()
    if(BUILD_TESTING)
        kv_test_target(vector_value_test vector_value)
        kv_test_target(hnsw_test hnsw)
        kv_test_target(vector_db_test vector_db)
        kv_test_target(vector_db_failure_test vector_db)
        set_tests_properties(vector_value_test hnsw_test vector_db_test vector_db_failure_test PROPERTIES LABELS vector)
        if(KV_SANITIZERS MATCHES "(^|,)thread(,|$)")
            target_compile_definitions(hnsw_test PRIVATE KV_TEST_THREAD_SANITIZED=1)
        endif()
        if(KV_ENABLE_RECALL_REGRESSION)
            foreach(dimension 128 768)
                add_test(NAME hnsw_recall_20k_${dimension}
                    COMMAND hnsw_recall_probe regression 20000 ${dimension} 32 200 100)
                set_tests_properties(hnsw_recall_20k_${dimension} PROPERTIES LABELS "vector;recall;slow" TIMEOUT 900)
            endforeach()
        endif()
        add_test(NAME vector_bench_maintenance COMMAND vector_bench --mode write
            --path "${CMAKE_CURRENT_BINARY_DIR}/vector_maintenance_bench"
            --vectors 100 --dimension 8 --write-batch 8
            --memtable-limit 16 --l0-limit 1 --require-maintenance --reset)
        add_test(NAME vector_bench_smoke COMMAND vector_bench --mode all
            --path "${CMAKE_CURRENT_BINARY_DIR}/week6_vector_bench"
            --vectors 100 --dimension 8 --queries 8 --recall-queries 2
            --mixed-operations 20 --write-batch 8 --query-batch 4 --ef-search 32 --reset)
        set_tests_properties(vector_bench_maintenance vector_bench_smoke PROPERTIES LABELS "vector;benchmark" TIMEOUT 60)
    endif()
endif()
