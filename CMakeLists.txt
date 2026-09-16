cmake_minimum_required(VERSION 3.16)
project(mini_lsm_kv VERSION 0.1 LANGUAGES CXX)

include(CTest)
option(KV_ENABLE_RECALL_REGRESSION "Run 20k-vector recall regressions (slow)" OFF)
option(KV_ENABLE_SIMD "Enable supported vector-distance SIMD backends" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Sanitizers.cmake)

add_library(kvdb
    src/block_cache.cc
    src/bloom_filter.cc
    src/db.cc
    src/format.cc
    src/hnsw_index.cc
    src/manifest.cpp
    src/memtable.cc
    src/sstable.cpp
    src/skiplist.cc
    src/vector_value.cc
    src/wal.cc
    src/write_batch.cc
)

target_include_directories(kvdb PUBLIC include)
target_compile_options(kvdb PRIVATE -Wall -Wextra -Wpedantic)
if(NOT KV_ENABLE_SIMD)
    target_compile_definitions(kvdb PRIVATE KV_DISABLE_SIMD=1)
endif()

if(BUILD_TESTING)
    add_executable(kv_test test/kv_test.cc)
    target_link_libraries(kv_test PRIVATE kvdb)
    target_compile_definitions(kv_test PRIVATE
        KV_TEST_DB_ROOT="${CMAKE_CURRENT_BINARY_DIR}/test_dbs")
    target_compile_options(kv_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME kv_test COMMAND kv_test)
    set_tests_properties(kv_test PROPERTIES TIMEOUT 60)
    if(KV_SANITIZERS MATCHES "(^|,)thread(,|$)")
        set_tests_properties(kv_test PROPERTIES TIMEOUT 300)
    endif()

    add_executable(hnsw_test test/hnsw_test.cc)
    target_link_libraries(hnsw_test PRIVATE kvdb)
    target_compile_options(hnsw_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME hnsw_test COMMAND hnsw_test)
    set_tests_properties(hnsw_test PROPERTIES TIMEOUT 60)
    if(KV_SANITIZERS MATCHES "(^|,)thread(,|$)")
        target_compile_definitions(hnsw_test PRIVATE KV_TEST_THREAD_SANITIZED=1)
        set_tests_properties(hnsw_test PROPERTIES TIMEOUT 300)
    endif()

    add_test(NAME week5_bench_sync_smoke
        COMMAND week5_bench --mode sync --operations 10000 --threads 2 --batch-size 8)
    set_tests_properties(week5_bench_sync_smoke PROPERTIES
        LABELS benchmark
        TIMEOUT 60)
endif()

add_executable(kv_bench bench/kv_bench.cpp)
target_link_libraries(kv_bench PRIVATE kvdb)
target_compile_options(kv_bench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(week5_bench bench/week5_bench.cpp)
target_link_libraries(week5_bench PRIVATE kvdb)
target_compile_options(week5_bench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(vector_bench bench/vector_bench.cpp)
target_link_libraries(vector_bench PRIVATE kvdb)
target_compile_options(vector_bench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(hnsw_recall_probe tools/hnsw_recall_probe.cpp)
target_link_libraries(hnsw_recall_probe PRIVATE kvdb)
target_compile_options(hnsw_recall_probe PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_stress stress/kv_stress.cpp)
target_link_libraries(kv_stress PRIVATE kvdb)
target_compile_options(kv_stress PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    if(KV_ENABLE_RECALL_REGRESSION)
        foreach(dimension 128 768)
            add_test(NAME hnsw_recall_20k_${dimension}
                COMMAND hnsw_recall_probe regression 20000 ${dimension} 32 200 100)
            set_tests_properties(hnsw_recall_20k_${dimension} PROPERTIES
                LABELS "recall;slow" TIMEOUT 900)
        endforeach()
    endif()
    add_test(NAME vector_bench_maintenance
        COMMAND vector_bench --mode write
            --path "${CMAKE_CURRENT_BINARY_DIR}/vector_maintenance_bench"
            --vectors 100 --dimension 8 --write-batch 8
            --memtable-limit 16 --l0-limit 1 --require-maintenance --reset)
    set_tests_properties(vector_bench_maintenance PROPERTIES
        LABELS benchmark TIMEOUT 60)

    add_test(NAME vector_bench_smoke
        COMMAND vector_bench
            --mode all
            --path "${CMAKE_CURRENT_BINARY_DIR}/week6_vector_bench"
            --vectors 100
            --dimension 8
            --queries 8
            --recall-queries 2
            --mixed-operations 20
            --write-batch 8
            --query-batch 4
            --ef-search 32
            --reset)
    set_tests_properties(vector_bench_smoke PROPERTIES
        LABELS benchmark
        TIMEOUT 60)

    add_test(NAME kv_stress_fixed_seed
        COMMAND kv_stress
            --path "${CMAKE_CURRENT_BINARY_DIR}/stress_dbs/kv_stress_fixed_seed"
            --epochs 2
            --threads 4
            --ops-per-thread 250
            --keys-per-thread 128
            --value-size 64
            --delete-percent 25
            --batch-size 4
            --reopen-every-epochs 1
            --compact-every-epochs 1
            --seed 20260712
            --memtable-limit 32
            --l0-limit 2)
    set_tests_properties(kv_stress_fixed_seed PROPERTIES
        LABELS stress
        TIMEOUT 120)
endif()
