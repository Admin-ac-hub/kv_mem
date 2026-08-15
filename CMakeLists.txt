cmake_minimum_required(VERSION 3.16)
project(mini_lsm_kv VERSION 0.1 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Sanitizers.cmake)

add_library(kvdb
    src/block_cache.cc
    src/bloom_filter.cc
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

if(BUILD_TESTING)
    add_executable(kv_test test/kv_test.cc)
    target_link_libraries(kv_test PRIVATE kvdb)
    target_compile_definitions(kv_test PRIVATE
        KV_TEST_DB_ROOT="${CMAKE_CURRENT_BINARY_DIR}/test_dbs")
    target_compile_options(kv_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME kv_test COMMAND kv_test)
    set_tests_properties(kv_test PROPERTIES TIMEOUT 60)
endif()

add_executable(kv_bench bench/kv_bench.cpp)
target_link_libraries(kv_bench PRIVATE kvdb)
target_compile_options(kv_bench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_stress stress/kv_stress.cpp)
target_link_libraries(kv_stress PRIVATE kvdb)
target_compile_options(kv_stress PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
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
