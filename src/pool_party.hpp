#ifndef POOL_PARTY_HPP
#define POOL_PARTY_HPP

#include <condition_variable>
#include <cuchar>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>

namespace pool_party {

using size_t = std::size_t;

// This class is needed to store generic callables that are not copyable due to
// move-only semantics for their captures. The class is a type-erased,
// movable-only generic callable.
class movable_callable {
  private:
    struct impl_base {
        virtual void call() = 0;
        virtual ~impl_base() = default;

        impl_base() = default;
        impl_base(const impl_base&) = delete;
        impl_base(impl_base&&) = default;
        impl_base& operator=(const impl_base&) = delete;
        impl_base& operator=(impl_base&&) = default;
    };

    std::unique_ptr<impl_base> impl_;

    template <typename Callable> struct impl_derived : impl_base {
        Callable func_;
        impl_derived(Callable&& func) : func_(std::move(func)) {}
        void call() override { std::invoke(func_); }
    };

  public:
    template <typename Callable>
    movable_callable(Callable&& function)
        : impl_(std::make_unique<impl_derived<Callable>>(std::move(function))) {
    }
    ~movable_callable() = default;
    movable_callable() = default;
    movable_callable(const movable_callable&) = delete;
    movable_callable(movable_callable&) = delete;
    movable_callable(movable_callable&& other) noexcept
        : impl_(std::move(other.impl_)) {}
    movable_callable& operator=(movable_callable&& other) noexcept {
        impl_ = std::move(other.impl_);
        return *this;
    }
    movable_callable& operator=(const movable_callable&) = delete;

    void operator()() { impl_->call(); }
}; // class movable_callable

class thread_pool {
  public:
    explicit thread_pool()
        : threads_(std::thread::hardware_concurrency()),
          num_threads_(std::thread::hardware_concurrency()) {}
    explicit thread_pool(size_t num_threads)
        : threads_(num_threads), num_threads_(num_threads) {}

    ~thread_pool() { stop(); }

    thread_pool(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    void start() {
        // Double check lock to avoid needing to lock if the threads are already
        // running
        if (is_running_) {
            return;
        }

        std::unique_lock pool_lock(thread_pool_lock_);
        if (is_running_) {
            return;
        }

        threads_ = std::vector<std::jthread>(num_threads_);
        for (size_t i = 0; i < num_threads_; i++) {
            threads_[i] = std::jthread{&thread_pool::worker_task, this, i};
        }
        is_running_ = true;
    }

    void stop() {
        // Double check lock to avoid needing to lock if the threads are already
        // running
        if (!is_running_) {
            return;
        }
        std::unique_lock pool_lock(thread_pool_lock_);

        if (!is_running_) {
            return;
        }

        std::unique_lock q_lock(queue_lock_);
        shutting_down_ = true;
        q_lock.unlock();

        queue_cv_.notify_all();
        for (auto& thread : threads_) {
            thread.join();
        }
        threads_.clear();
        shutting_down_ = false;
        is_running_ = false;
    }

    [[nodiscard]] size_t num_threads() const { return num_threads_; }

    [[nodiscard]] bool is_running() const { return is_running_; }

    template <typename Callable, typename... Args,
              typename ReturnType = std::invoke_result_t<Callable, Args...>>
    [[nodiscard]] auto submit(Callable&& task, Args&&... args) {
        std::promise<ReturnType> task_promise;
        std::future<ReturnType> task_future = task_promise.get_future();
        std::unique_lock q_lock(queue_lock_);
        if constexpr (std::is_same_v<ReturnType, void>) {

            task_queue_.emplace(
                [task_promise = std::move(task_promise),
                 task = std::forward<Callable>(task),
                 ... args = std::forward<Args>(args)]() mutable {
                    std::invoke(std::forward<Callable>(task),
                                std::forward<Args>(args)...);
                    task_promise.set_value();
                });
        } else {
            task_queue_.emplace([task_promise = std::move(task_promise),
                                 task = std::forward<Callable>(task),
                                 ... args =
                                     std::forward<Args>(args)]() mutable {
                task_promise.set_value(std::invoke(
                    std::forward<Callable>(task), std::forward<Args>(args)...));
            });
        }
        q_lock.unlock();
        queue_cv_.notify_one();
        return task_future;
    }

  private:
    void worker_task(size_t thread_num) {
        while (true) {
            std::unique_lock q_lock(queue_lock_);
            queue_cv_.wait(q_lock, [this] {
                return shutting_down_ || !task_queue_.empty();
            });

            if (shutting_down_) {
                return;
            }

            auto task = std::move(task_queue_.front());
            task_queue_.pop();
            task();
        }
    }

    std::condition_variable queue_cv_;
    std::mutex queue_lock_;
    std::mutex thread_pool_lock_;
    std::queue<movable_callable> task_queue_;
    std::vector<std::jthread> threads_;
    size_t num_threads_;

    // atomic so it can be read outside of thread_pool_lock
    std::atomic<bool> is_running_ = false;
    bool shutting_down_ = false;
}; // class thread_pool

} // namespace pool_party

#endif // POOL_PARTY_HPP
