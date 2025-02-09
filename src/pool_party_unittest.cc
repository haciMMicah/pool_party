#include "pool_party.hpp"

#include <exception>
#include <future>
#include <gtest/gtest.h>

namespace {
int free_func() { return 42; }
int free_func_with_args(int arg) { return arg; }
} // namespace

class ThreadPoolTestWithParam : public testing::TestWithParam<std::size_t> {};

INSTANTIATE_TEST_SUITE_P(ThreadNums, ThreadPoolTestWithParam,
                         testing::Values(1, 2, 4, 8, 16, 32, 64, 128));

TEST(MovableCallableTest, TestUsesSBO) {
    std::promise<int> promise{};
    std::future<int> future = promise.get_future();
    auto lambda = [promise = std::move(promise)]() mutable {
        promise.set_value(42);
    };
    // Add sizeof(void*) to the lambda to account for vptr in the wrapper.
    pool_party::movable_callable<sizeof(lambda) + sizeof(void*), 8UL> callable(
        std::move(lambda));

    callable();
    EXPECT_EQ(future.get(), 42);
}

TEST(MovableCallableTest, TestUsesHeap) {
    std::promise<int> promise{};
    std::future<int> future = promise.get_future();
    pool_party::movable_callable<0UL, 8UL> callable(
        [promise = std::move(promise)]() mutable { promise.set_value(42); });

    callable();
    EXPECT_EQ(future.get(), 42);
}

TEST(MovableCallableTest, TestUsesHeapAndSBO) {
    std::promise<int> promise{};
    std::future<int> future = promise.get_future();
    auto lambda = [promise = std::move(promise)]() mutable {
        promise.set_value(42);
    };
    // Add sizeof(void*) to the lambda to account for vptr in the wrapper.
    pool_party::movable_callable<sizeof(lambda) + sizeof(void*), 8UL> callable(
        std::move(lambda));

    callable();
    EXPECT_EQ(future.get(), 42);

    int extra_data = 42;
    std::promise<int> promise2{};
    std::future<int> future2 = promise2.get_future();
    callable = [promise2 = std::move(promise2), extra_data]() mutable {
        promise2.set_value(extra_data);
    };

    callable();
    EXPECT_EQ(future2.get(), 42);
}

TEST(ThreadPoolTest, TestConstructors) {
    pool_party::thread_pool pool1{};
    pool1.start();
    EXPECT_EQ(pool1.num_threads(), std::thread::hardware_concurrency());

    pool_party::thread_pool pool2{2};
    pool2.start();
    EXPECT_EQ(pool2.num_threads(), 2);
}

TEST(ThreadPoolTest, TestSubmitSetsExceptions) {
    pool_party::thread_pool pool{};
    pool.start();
    struct TestException : public std::exception {};
    auto exception_fut = pool.submit([] { throw TestException(); });

    EXPECT_THROW({ exception_fut.get(); }, TestException);
}

TEST(ThreadPoolTest, TestSubmitWithCallbackSetsExceptions) {
    pool_party::thread_pool pool{};
    pool.start();
    struct TestException : public std::exception {};
    pool.submit_with_callback(
        [] { throw TestException(); },
        [](std::exception_ptr e_ptr) noexcept { EXPECT_TRUE(e_ptr); });
}

TEST(ThreadPoolTest, TestSubmitDetached) {
    // Test with rvalue lambdas with args and task return value.
    pool_party::thread_pool pool{};
    pool.start();
    std::promise<int> promise{};
    std::future<int> task_return_value = promise.get_future();
    pool.submit_detached(
        [promise = std::move(promise)](int returned_value) mutable {
            promise.set_value(returned_value);
        },
        42);
    EXPECT_EQ(task_return_value.get(), 42);

    // Test with moving lvalue lambdas with args.
    std::promise<int> promise2{};
    std::future<int> task_return_value2 = promise2.get_future();
    auto task_lambda = [promise =
                            std::move(promise2)](int passed_in_value) mutable {
        promise.set_value(passed_in_value);
    };
    pool.submit_detached(std::move(task_lambda), 42);
    EXPECT_EQ(task_return_value2.get(), 42);

    // Test with rvalue lambdas without args or return values.
    std::promise<bool> task_called_promise;
    std::future<bool> task_called_fut = task_called_promise.get_future();
    pool.submit_detached(
        [task_called_promise = std::move(task_called_promise)]() mutable {
            task_called_promise.set_value(true);
        });
    EXPECT_EQ(task_called_fut.get(), true);
}

TEST(ThreadPoolTest, TestSubmitWithCallback) {
    // Test with rvalue lambdas with args and task return value.
    pool_party::thread_pool pool{};
    pool.start();
    std::promise<int> promise{};
    std::future<int> task_return_value = promise.get_future();
    pool.submit_with_callback(
        [](int task_arg_value) mutable { return task_arg_value * 2; },
        [promise = std::move(promise)](std::exception_ptr,
                                       int returned_value) mutable noexcept {
            promise.set_value(returned_value);
        },
        42);
    EXPECT_EQ(task_return_value.get(), 42 * 2);

    // Test with moving lvalue lambdas with args and task return value.
    std::promise<int> promise2{};
    std::future<int> task_return_value2 = promise2.get_future();
    auto task_lambda = [](int task_arg_value) mutable {
        return task_arg_value * 2;
    };
    auto callback_lambda =
        [promise = std::move(promise2)](std::exception_ptr,
                                        int returned_value) mutable noexcept {
            promise.set_value(returned_value);
        };
    pool.submit_with_callback(std::move(task_lambda),
                              std::move(callback_lambda), 42);
    EXPECT_EQ(task_return_value2.get(), 42 * 2);

    // Test with rvalue lambdas without args or return values.
    std::promise<bool> task_called_promise;
    std::future<bool> task_called_fut = task_called_promise.get_future();
    std::promise<bool> callback_called_promise{};
    std::future<bool> callback_called_fut =
        callback_called_promise.get_future();
    pool.submit_with_callback(
        [task_called_promise = std::move(task_called_promise)]() mutable {
            task_called_promise.set_value(true);
        },
        [callback_called_promise = std::move(callback_called_promise)](
            std::exception_ptr) mutable noexcept {
            callback_called_promise.set_value(true);
        });
    EXPECT_EQ(task_called_fut.get(), true);
    EXPECT_EQ(callback_called_fut.get(), true);
}

TEST(ThreadPoolTest, TestUsesSBO) {
    // Test a thread pool that allows 16 bytes of buffer space and 8 bytes
    // of alignment for SBO.
    pool_party::thread_pool<16UL, 8UL> pool{};
    pool.start();
    std::promise<int> promise{};
    std::future<int> task_return_value = promise.get_future();
    pool.submit_detached(
        [promise = std::move(promise)](int returned_value) mutable {
            promise.set_value(returned_value);
        },
        42);
    EXPECT_EQ(task_return_value.get(), 42);
}

TEST(ThreadPoolTest, TestUsesHeap) {
    // Test a thread pool that always uses the heap.
    pool_party::thread_pool<0UL> pool{};
    pool.start();
    std::promise<int> promise{};
    std::future<int> task_return_value = promise.get_future();
    pool.submit_detached(
        [promise = std::move(promise)](int returned_value) mutable {
            promise.set_value(returned_value);
        },
        42);
    EXPECT_EQ(task_return_value.get(), 42);
}

TEST_P(ThreadPoolTestWithParam, TestStart) {
    pool_party::thread_pool pool{GetParam()};
    auto fut1 = pool.submit([] { return 42; });
    EXPECT_FALSE(pool.is_running());
    pool.start();
    EXPECT_TRUE(pool.is_running());
    EXPECT_EQ(fut1.get(), 42);
}

TEST_P(ThreadPoolTestWithParam, TestStop) {
    auto num_threads = GetParam();
    pool_party::thread_pool pool{num_threads};
    pool.start();
    EXPECT_TRUE(pool.is_running());
    EXPECT_EQ(pool.num_threads(), num_threads);
    EXPECT_EQ(pool.submit([] { return 42; }).get(), 42);
    pool.stop();
    EXPECT_FALSE(pool.is_running());
}

TEST_P(ThreadPoolTestWithParam, TestRestart) {
    pool_party::thread_pool pool{GetParam()};
    EXPECT_FALSE(pool.is_running());
    pool.start();
    EXPECT_TRUE(pool.is_running());
    EXPECT_EQ(pool.submit([] { return 42; }).get(), 42);
    pool.stop();
    EXPECT_FALSE(pool.is_running());
    auto fut1 = pool.submit([] { return 42; });
    pool.start();
    EXPECT_TRUE(pool.is_running());
    EXPECT_EQ(fut1.get(), 42);
}

TEST_P(ThreadPoolTestWithParam, TestSubmit) {
    pool_party::thread_pool pool{GetParam()};
    pool.start();

    // Test submitting lambdas.
    auto task = [] { return 1; };
    auto task2 = [](int i) { return i; };
    auto fut1 = pool.submit(task);
    auto fut2 = pool.submit(task2, 2);
    EXPECT_EQ(fut1.get(), 1);
    EXPECT_EQ(fut2.get(), 2);

    // Test submitting member functions.
    struct test_struct {
        int bar = 42;
        int foo() const { return bar; }
    };

    test_struct struct_test;
    auto fut3 = pool.submit(&test_struct::foo, &struct_test);
    EXPECT_EQ(fut3.get(), 42);

    // Test submitting free functions.
    auto fut4 = pool.submit(&free_func);
    auto fut5 = pool.submit(&free_func_with_args, 43);
    EXPECT_EQ(fut4.get(), 42);
    EXPECT_EQ(fut5.get(), 43);
}

TEST_P(ThreadPoolTestWithParam, TestTasksPerThread) {
    pool_party::thread_pool pool{GetParam()};

    std::size_t num_threads = GetParam();
    std::size_t expected_total = 0;
    std::vector<std::future<std::size_t>> futures;
    futures.reserve(num_threads);
    for (std::size_t task_num = 1; task_num <= num_threads; task_num++) {
        futures.emplace_back(
            pool.submit([](auto var) { return var; }, task_num));
        expected_total += task_num;
    }

    pool.start();
    std::size_t actual_total = 0;
    for (auto& fut : futures) {
        actual_total += fut.get();
    }

    EXPECT_EQ(expected_total, actual_total);
}

TEST_P(ThreadPoolTestWithParam, TestManyTasks) {
    pool_party::thread_pool pool{GetParam()};

    std::vector<std::future<std::size_t>> futures;
    std::size_t num_tasks = 1'000'000;
    futures.reserve(num_tasks);
    for (std::size_t task_num = 1; task_num <= num_tasks; task_num++) {
        futures.emplace_back(
            pool.submit([] { return static_cast<std::size_t>(1); }));
    }

    pool.start();
    std::size_t actual_total = 0;
    for (auto& fut : futures) {
        actual_total += fut.get();
    }

    EXPECT_EQ(num_tasks, actual_total);
}
