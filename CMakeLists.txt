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
target_link_libraries(
    pool_party_tests
    GTest::gtest_main
)
target_compile_options(
    pool_party_tests
    PUBLIC
    "-W;-Wall;-Werror;-Wpedantic;-Wextra;-g"
)

add_executable(
    pool_party_performance_tests
    src/pool_party_performance_tests.cc
)
target_compile_options(
    pool_party_performance_tests
    PUBLIC
    "-W;-Wall;-Werror;-Wpedantic;-Wextra;-O3"
)

add_executable(
    thread_pool_performance_tests
    src/thread_pool_performance_tests.cc
)
target_include_directories(
    thread_pool_performance_tests
    PUBLIC
    include
)
target_compile_options(
    thread_pool_performance_tests
    PUBLIC
    "-W;-Wall;-Werror;-Wpedantic;-Wextra;-O3"
)
add_compile_definitions(BS_THREAD_POOL_ENABLE_PAUSE=1)
include(GoogleTest)
gtest_discover_tests(pool_party_tests)
