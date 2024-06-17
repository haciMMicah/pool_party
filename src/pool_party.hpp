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
    movable_callable(movable_callable&& other)
        : impl_(std::move(other.impl_)) {}
    movable_callable& operator=(movable_callable&& other) {
        impl_ = std::move(other.impl_);
        return *this;
    }
    movable_callable& operator=(const movable_callable&) = delete;

    void operator()() { impl_->call(); }
}; // class movable_callable

template <size_t PoolSize = 0> class thread_pool {
  public:
    explicit thread_pool()
        : num_threads_(PoolSize > 0 ? PoolSize
                                    : std::thread::hardware_concurrency()),
          threads_(num_threads_) {}

    ~thread_pool() { stop(); }

    thread_pool(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = default;

    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool& operator=(thread_pool&&) = default;

    void start() {
        for (size_t i = 0; i < num_threads_; i++) {
            threads_[i] = std::jthread{&thread_pool::worker_task, this, i};
        }
    }

    void stop() {
        {
            // We need to lock for this test and set even though shutting_down_
            // is atomic because we only want one call to stop() to begin the
            // shutdown process
            std::lock_guard lk(shutdown_lock_);
            if (shutting_down_)
                return;
            shutting_down_ = true;
        }
        queue_cv_.notify_all();
        for (auto& thread : threads_) {
            thread.join();
        }
    }

    size_t num_threads() const { return num_threads_; }

    template <typename Callable, typename... Args,
              typename ReturnType = std::invoke_result_t<Callable, Args...>>
    [[nodiscard]] auto submit(Callable&& task, Args&&... args) {
        std::promise<ReturnType> task_promise;
        std::future<ReturnType> task_future = task_promise.get_future();
        std::unique_lock lk(queue_lock_);
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
        lk.unlock();
        queue_cv_.notify_one();
        return task_future;
    }

  private:
    void worker_task(size_t thread_num) {
        while (true) {
            std::unique_lock lk(queue_lock_);
            queue_cv_.wait(lk, [this] {
                return shutting_down_ || task_queue_.size() > 0;
            });

            if (shutting_down_)
                return;

            auto task = std::move(task_queue_.front());
            task_queue_.pop();
            task();
        }
    }

    size_t num_threads_;
    std::vector<std::jthread> threads_;
    std::atomic<bool> shutting_down_ = false;
    std::mutex shutdown_lock_;
    std::queue<movable_callable> task_queue_;
    std::mutex queue_lock_;
    std::condition_variable queue_cv_;
}; // class thread_pool

} // namespace pool_party

#endif // POOL_PARTY_HPP
