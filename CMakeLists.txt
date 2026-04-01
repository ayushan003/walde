cmake_minimum_required(VERSION 3.20)
project(walde
    VERSION 1.0.0
    DESCRIPTION "W-LFU Multi-Level Cache Engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WALDE_BUILD_TESTS      "Build unit tests"        ON)
option(WALDE_BUILD_BENCHMARKS "Build benchmark harness" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake_unofficial
)

FetchContent_MakeAvailable(googletest xxhash)

# ─── Core library ──────────────────────────────────────────────
add_library(walde_core STATIC
    src/slab_allocator.cpp
    src/lru_list.cpp
    src/count_min_sketch.cpp
    src/cache_stripe.cpp
    src/striped_cache.cpp
    src/storage_backend.cpp
    src/cache_engine.cpp
    src/demotion_queue.cpp
    src/demotion_drainer.cpp
    src/workload_generator.cpp
)

target_include_directories(walde_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(walde_core PUBLIC
    xxhash
)

# ─── Benchmark harness ─────────────────────────────────────────
if(WALDE_BUILD_BENCHMARKS)
    add_executable(walde_benchmark benchmarks/benchmark_main.cpp)
    target_include_directories(walde_benchmark PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/benchmarks
    )
    target_link_libraries(walde_benchmark PRIVATE walde_core)
endif()

# ─── Tests ─────────────────────────────────────────────────────
if(WALDE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
