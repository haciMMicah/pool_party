cmake_minimum_required(VERSION 3.14)
project(pool_party)

# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(
    pool_party_tests
    src/pool_party_unittest.cc
)
add_executable(
    pool_party_performance_tests
    src/pool_party_performance_tests.cc
)
set_target_properties(
    pool_party_performance_tests
    PROPERTIES
    COMPILE_FLAGS
    "-O3"
)
target_link_libraries(
    pool_party_tests
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(pool_party_tests)
