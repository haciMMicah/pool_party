#include "pool_party.hpp"

#include <gtest/gtest.h>
#include <ranges>

namespace {
int free_func() { return 42; }
int free_funct_with_args(int arg) { return arg; }
} // namespace

class ThreadPoolTestWithParam : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(ThreadNums, ThreadPoolTestWithParam,
                         testing::Values(1, 2, 4, 8, 16, 32, 64, 128));

TEST(ThreadPoolTest, TestConstructors) {
    pool_party::thread_pool pool1{};
    pool1.start();
    EXPECT_EQ(pool1.num_threads(), std::thread::hardware_concurrency());

    pool_party::thread_pool pool2{2};
    pool2.start();
    EXPECT_EQ(pool2.num_threads(), 2);
}

TEST_P(ThreadPoolTestWithParam, TestStart) {
    // TODO: This test is garbage, should come up with a better way to test
    // start()
    using namespace std::chrono_literals;
    pool_party::thread_pool pool{GetParam()};
    auto fut1 = pool.submit([] { return 42; });
    EXPECT_EQ(fut1.wait_for(1s), std::future_status::timeout);
    pool.start();
    EXPECT_EQ(fut1.get(), 42);
}

TEST_P(ThreadPoolTestWithParam, TestStop) {
    auto num_threads = GetParam();
    pool_party::thread_pool pool{num_threads};
    pool.start();
    EXPECT_EQ(pool.num_threads(), num_threads);
    EXPECT_EQ(pool.submit([] { return 42; }).get(), 42);
    pool.stop();
}

TEST_P(ThreadPoolTestWithParam, TestRestart) {
    pool_party::thread_pool pool{GetParam()};
    pool.start();
    EXPECT_EQ(pool.submit([] { return 42; }).get(), 42);
    pool.stop();
    auto fut1 = pool.submit([] { return 42; });
    pool.start();
    EXPECT_EQ(fut1.get(), 42);
}

TEST_P(ThreadPoolTestWithParam, TestSubmit) {
    pool_party::thread_pool pool{GetParam()};
    pool.start();

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

TEST_P(ThreadPoolTestWithParam, TestTasksPerThread) {
    pool_party::thread_pool pool{GetParam()};
    pool.start();

    std::size_t num_threads = GetParam();
    std::size_t expected_total = 0;
    std::vector<std::future<std::size_t>> futures;
    futures.reserve(num_threads);
    for (std::size_t task_num = 1; task_num <= num_threads; task_num++) {
        futures.emplace_back(
            pool.submit([](auto var) { return var; }, task_num));
        expected_total += task_num;
    }

    std::size_t actual_total = 0;
    for (auto& fut : futures) {
        actual_total += fut.get();
    }

    EXPECT_EQ(expected_total, actual_total);
}

TEST_P(ThreadPoolTestWithParam, TestManyTasks) {
    pool_party::thread_pool pool{GetParam()};
    pool.start();

    std::size_t num_threads = GetParam();
    std::vector<std::future<std::size_t>> futures;
    std::size_t num_tasks = 1'000'000;
    futures.reserve(num_tasks);
    for (std::size_t task_num = 1; task_num <= num_tasks; task_num++) {
        futures.emplace_back(
            pool.submit([] { return static_cast<std::size_t>(1); }));
    }

    std::size_t actual_total = 0;
    for (auto& fut : futures) {
        actual_total += fut.get();
    }

    EXPECT_EQ(num_tasks, actual_total);
}
