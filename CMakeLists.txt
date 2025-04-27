cmake_minimum_required(VERSION 3.10)
project(MatchingEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Threads
find_package(Threads REQUIRED)

# Ensure GCC 11 or higher
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.0)
    message(FATAL_ERROR "GCC 11 or higher is required. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
endif()

include_directories(${PROJECT_SOURCE_DIR}/include)

# Main executable
add_executable(matching_engine
    src/matching_engine.cpp
    src/main.cpp
)
target_link_libraries(matching_engine pthread) # Add pthread

# Unit tests
find_package(GTest REQUIRED)
add_executable(test_matching_engine
    tests/test.cpp
    src/matching_engine.cpp
)
target_link_libraries(test_matching_engine GTest::GTest GTest::Main Threads::Threads)

enable_testing()
add_test(NAME MatchingEngineTests COMMAND test_matching_engine)
