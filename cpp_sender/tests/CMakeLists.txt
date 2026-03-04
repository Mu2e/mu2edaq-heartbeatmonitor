# ---------------------------------------------------------------------------
# GoogleTest — prefer system install, fall back to FetchContent
# ---------------------------------------------------------------------------
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found via find_package — fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
add_executable(test_heartbeat_sender_lib test_heartbeat_sender_lib.cpp)

target_link_libraries(test_heartbeat_sender_lib PRIVATE
    heartbeat_sender_lib
    GTest::gtest
    GTest::gtest_main
)

target_compile_options(test_heartbeat_sender_lib PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

include(GoogleTest)
gtest_discover_tests(test_heartbeat_sender_lib)
