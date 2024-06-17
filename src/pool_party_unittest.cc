#include "pool_party.hpp"
#include <gtest/gtest.h>

namespace {
int free_func() { return 42; }
int free_funct_with_args(int arg) { return arg; }
} // namespace

TEST(ThreadPoolTests, TestStart) {
    // TODO: This test is garbage, should come up with a better way to test
    // start()
    using namespace std::chrono_literals;
    pool_party::thread_pool pool;
    auto fut1 = pool.submit([] { return 42; });
    EXPECT_EQ(fut1.wait_for(1s), std::future_status::timeout);
    pool.start();
    EXPECT_EQ(fut1.get(), 42);
}

TEST(ThreadPoolTests, TestStop) {
    pool_party::thread_pool pool;
    pool.start();
    EXPECT_EQ(pool.num_threads(), std::thread::hardware_concurrency());
    EXPECT_EQ(pool.submit([] { return 42; }).get(), 42);
    pool.stop();
}

TEST(ThreadPoolTests, TestConstructors) {
    pool_party::thread_pool pool;
    pool.start();
    EXPECT_EQ(pool.num_threads(), std::thread::hardware_concurrency());
}

TEST(ThreadPoolTests, TestRestart) {
    pool_party::thread_pool pool;
    pool.start();
    EXPECT_EQ(pool.submit([] { return 42; }).get(), 42);
    pool.stop();
    auto fut1 = pool.submit([] { return 42; });
    pool.start();
    EXPECT_EQ(fut1.get(), 42);
}

TEST(ThreadPoolTests, TestSubmit) {
    constexpr std::size_t num_threads = 4;
    pool_party::thread_pool<num_threads> pool;
    pool.start();
    EXPECT_EQ(pool.num_threads(), 4);

    // Test submitting lambdas
    auto task = [] { return 1; };
    auto task2 = [](int i) { return i; };
    auto fut1 = pool.submit(task);
    auto fut2 = pool.submit(task2, 2);
    EXPECT_EQ(fut1.get(), 1);
    EXPECT_EQ(fut2.get(), 2);

    // Test submitting member functions
    struct test_struct {
        int bar = 42;
        int foo() { return bar; }
    };

    test_struct struct_test;
    auto fut3 = pool.submit(&test_struct::foo, &struct_test);
    EXPECT_EQ(fut3.get(), 42);

    // Test submitting free functions
    auto fut4 = pool.submit(&free_func);
    auto fut5 = pool.submit(&free_funct_with_args, 43);
    EXPECT_EQ(fut4.get(), 42);
    EXPECT_EQ(fut5.get(), 43);
}
