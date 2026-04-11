cmake_minimum_required(VERSION 3.20)

set(TEST_SOURCES
    test_slab_allocator.cpp
    test_lru_list.cpp
    test_cms.cpp
    test_cache_stripe.cpp
    test_striped_cache.cpp
    test_demotion_queue.cpp
    test_demotion_drainer.cpp
    test_cache_engine.cpp
    test_trace_loader.cpp
)

add_executable(walde_tests ${TEST_SOURCES})

target_link_libraries(walde_tests
    PRIVATE
        walde_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(walde_tests)
