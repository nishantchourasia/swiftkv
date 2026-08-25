cmake_minimum_required(VERSION 3.16)
project(swiftkv VERSION 0.1.0 LANGUAGES CXX)

# C++20 is required, not merely preferred: the code uses concepts, ranges and
# std::jthread. Note that std::format is deliberately NOT used -- the target
# compiler here is gcc 11, which did not ship <format> until gcc 13.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# Sanitizers are opt-in. The concurrency tests are run under ThreadSanitizer
# separately, because a data race that never happens to fire is still a bug and
# ordinary test runs will not find it.
option(SWIFTKV_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(SWIFTKV_TSAN "Build with ThreadSanitizer" OFF)

if(SWIFTKV_ASAN AND SWIFTKV_TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled at the same time")
endif()

if(SWIFTKV_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

if(SWIFTKV_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(swiftkv_core
    src/lru.cpp
    src/store.cpp
    src/protocol.cpp
    src/commands.cpp
    src/net.cpp
    src/server.cpp
    src/client.cpp
)
target_include_directories(swiftkv_core PUBLIC include)
target_link_libraries(swiftkv_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
