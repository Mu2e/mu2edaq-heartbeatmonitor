cmake_minimum_required(VERSION 3.14)
project(HeartbeatSender CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Dependency: nlohmann/json (for JSON config file parsing)
# Prefers a system-installed package; falls back to FetchContent.
# ---------------------------------------------------------------------------
find_package(nlohmann_json QUIET)

if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann/json not found via find_package — fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install    OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(heartbeat_sender_lib STATIC heartbeat_sender_lib.cpp)

target_include_directories(heartbeat_sender_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(heartbeat_sender_lib PUBLIC nlohmann_json::nlohmann_json)

target_compile_options(heartbeat_sender_lib PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(heartbeat_sender heartbeat_sender.cpp)

target_link_libraries(heartbeat_sender PRIVATE heartbeat_sender_lib)

target_compile_options(heartbeat_sender PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS heartbeat_sender RUNTIME DESTINATION bin)
install(TARGETS heartbeat_sender_lib ARCHIVE DESTINATION lib)
install(FILES heartbeat_sender_lib.h DESTINATION include)
