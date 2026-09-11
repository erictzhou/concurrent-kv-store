cmake_minimum_required(VERSION 3.20)

project(
    concurrent_kv_store
    VERSION 0.6.0
    DESCRIPTION "Modern C++ key-value store with WAL persistence and snapshot recovery"
    LANGUAGES CXX
)

include(FetchContent)

option(CONCURRENT_KV_STORE_BUILD_TESTS "Build GoogleTest suites" ON)
option(CONCURRENT_KV_STORE_BUILD_BENCHMARKS "Build Google Benchmark suites" ON)
option(CONCURRENT_KV_STORE_ENABLE_TSAN "Build with ThreadSanitizer instrumentation" OFF)

find_package(Threads REQUIRED)

if(CONCURRENT_KV_STORE_ENABLE_TSAN)
    if(MSVC)
        message(WARNING "ThreadSanitizer is not configured for MSVC; ignoring CONCURRENT_KV_STORE_ENABLE_TSAN")
    else()
        add_library(concurrent_kv_store_tsan INTERFACE)
        target_compile_options(concurrent_kv_store_tsan INTERFACE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(concurrent_kv_store_tsan INTERFACE
            -fsanitize=thread
        )
    endif()
endif()

add_library(kv_store_core
    src/common/string_utils.cpp
    src/parser/cli_parser.cpp
    src/persistence/snapshot.cpp
    src/persistence/wal.cpp
    src/server/cli_server.cpp
    src/store/kv_store.cpp
)

target_compile_features(kv_store_core PUBLIC cxx_std_20)

target_include_directories(kv_store_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(kv_store_core
    PUBLIC
        CONCURRENT_KV_STORE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(kv_store_core
    PUBLIC
        Threads::Threads
)

if(TARGET concurrent_kv_store_tsan)
    target_link_libraries(kv_store_core PUBLIC concurrent_kv_store_tsan)
endif()

set_target_properties(kv_store_core PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(kv_store_core PRIVATE /W4)
else()
    target_compile_options(kv_store_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(kv_store src/main.cpp)
target_link_libraries(kv_store PRIVATE kv_store_core)
target_compile_features(kv_store PRIVATE cxx_std_20)
set_target_properties(kv_store PROPERTIES CXX_EXTENSIONS OFF)

if(CONCURRENT_KV_STORE_BUILD_TESTS)
    enable_testing()

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(kv_store_tests
        tests/unit/test_kv_store.cpp
        tests/unit/test_cli.cpp
        tests/unit/test_kv_store_concurrency.cpp
        tests/unit/test_snapshot.cpp
        tests/unit/test_wal.cpp
        tests/integration/test_recovery.cpp
        tests/helpers/file_utils.cpp
        tests/helpers/temp_dir.cpp
    )

    target_include_directories(kv_store_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
    )
    target_link_libraries(kv_store_tests PRIVATE kv_store_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(kv_store_tests)

    add_executable(kv_store_stress_tests
        tests/stress/test_concurrency.cpp
        tests/stress/test_stress.cpp
        tests/helpers/file_utils.cpp
        tests/helpers/temp_dir.cpp
    )

    target_include_directories(kv_store_stress_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
    )
    target_link_libraries(kv_store_stress_tests PRIVATE kv_store_core GTest::gtest_main)
    gtest_discover_tests(kv_store_stress_tests)
endif()

if(CONCURRENT_KV_STORE_BUILD_BENCHMARKS)
    add_executable(kv_store_scaling_benchmark
        benchmarks/scaling/kv_store_scaling.cpp
    )
    target_link_libraries(kv_store_scaling_benchmark PRIVATE kv_store_core)

    add_executable(kv_store_checkpoint_benchmark
        benchmarks/scaling/checkpoint_cliff.cpp
    )
    target_link_libraries(kv_store_checkpoint_benchmark PRIVATE kv_store_core)

    FetchContent_Declare(
        googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(kv_store_benchmark
        benchmarks/core_hot_path/kv_store_benchmark.cpp
    )

    target_link_libraries(kv_store_benchmark
        PRIVATE
            kv_store_core
            benchmark::benchmark_main
    )
endif()
