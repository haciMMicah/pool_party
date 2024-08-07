#ifndef POOL_PARTY_HPP
#define POOL_PARTY_HPP

#include <condition_variable>
#include <cuchar>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace pool_party {

using size_t = std::size_t;

// This class is needed to store generic callables that are not copyable due to
// move-only semantics for their captures. The class is a type-erased,
// movable-only generic callable that takes no arguments and returns void.
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

template <typename Queue>
concept thread_pool_queue =
    requires { typename Queue::value_type; } &&
    requires(Queue queue, const Queue c_queue,
             typename Queue::value_type&& value) {
        { queue.push(std::move(value)) } -> std::same_as<void>;
        {
            queue.pop(std::stop_token())
        } -> std::same_as<std::optional<typename Queue::value_type>>;
        { c_queue.size() } -> std::convertible_to<std::size_t>;
        { c_queue.empty() } -> std::same_as<bool>;
    };

template <typename T> class simple_thread_safe_queue {
  public:
    using value_type = T;
    void push(value_type&& value) {
        std::unique_lock lock(queue_lock_);
        queue_.push(std::forward<value_type>(value));
        lock.unlock();
        queue_cv_.notify_one();
    }

    std::optional<value_type> pop(std::stop_token stoken) {
        std::unique_lock lock(queue_lock_);
        queue_cv_.wait(lock, stoken, [this] { return !queue_.empty(); });

        if (stoken.stop_requested()) {
            return std::nullopt;
        }

        auto front = std::move(queue_.front());
        queue_.pop();
        return front;
    }

    std::size_t size() const {
        std::lock_guard lock(queue_lock_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard lock(queue_lock_);
        return queue_.empty();
    }

  private:
    mutable std::mutex queue_lock_;
    std::queue<value_type> queue_;
    std::condition_variable_any queue_cv_;
};

template <std::movable CallableWrapper = movable_callable,
          thread_pool_queue Container =
              simple_thread_safe_queue<CallableWrapper>>
class thread_pool {
  public:
    thread_pool()
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
        // running.
        if (is_running_) {
            return;
        }

        std::unique_lock pool_lock(thread_pool_lock_);
        if (is_running_) {
            return;
        }

        threads_ = std::vector<std::jthread>(num_threads_);
        for (size_t i = 0; i < num_threads_; i++) {
            threads_[i] =
                std::jthread{std::bind_front(&thread_pool::worker_task, this)};
        }
        is_running_ = true;
    }

    void stop() {
        // Double check lock to avoid needing to lock if the threads are already
        // running.
        if (!is_running_) {
            return;
        }
        std::unique_lock pool_lock(thread_pool_lock_);

        if (!is_running_) {
            return;
        }

        for (auto& thread : threads_) {
            thread.request_stop();
            thread.join();
        }
        threads_.clear();
        is_running_ = false;
    }

    [[nodiscard]] size_t num_threads() const {
        std::unique_lock pool_lock(thread_pool_lock_);
        return num_threads_;
    }

    [[nodiscard]] bool is_running() const { return is_running_; }

    template <typename Callable, typename... Args>
        requires std::is_invocable_v<std::decay_t<Callable>,
                                     std::decay_t<Args>...>
    void submit_detached(Callable&& task, Args&&... args) {
        task_queue_.push([task = std::forward<Callable>(task),
                          ... args = std::forward<Args>(args)]() mutable {
            try {
                std::invoke(std::forward<Callable>(task),
                            std::forward<Args>(args)...);
            } catch (...) {
                // TODO: Maybe call a user set callback to foward the exception.
            }
        });
    }

    template <typename Callable, typename... Args,
              typename ReturnType = std::invoke_result_t<Callable, Args...>>
    [[nodiscard]] auto submit(Callable&& task, Args&&... args) {
        std::promise<ReturnType> task_promise;
        std::future<ReturnType> task_future = task_promise.get_future();
        if constexpr (std::is_same_v<ReturnType, void>) {

            task_queue_.push([task_promise = std::move(task_promise),
                              task = std::forward<Callable>(task),
                              ... args = std::forward<Args>(args)]() mutable {
                try {
                    std::invoke(std::forward<Callable>(task),
                                std::forward<Args>(args)...);
                    task_promise.set_value();
                } catch (...) {
                    task_promise.set_exception(std::current_exception());
                }
            });
        } else {
            task_queue_.push([task_promise = std::move(task_promise),
                              task = std::forward<Callable>(task),
                              ... args = std::forward<Args>(args)]() mutable {
                try {
                    task_promise.set_value(
                        std::invoke(std::forward<Callable>(task),
                                    std::forward<Args>(args)...));
                } catch (...) {
                    task_promise.set_exception(std::current_exception());
                }
            });
        }
        return task_future;
    }

    template <typename Callable, typename CallbackClosure, typename... Args,
              typename ReturnType = std::invoke_result_t<Callable, Args...>>
    void submit_with_callback(Callable&& task, CallbackClosure&& callback,
                              Args&&... task_args) {
        // Call callback with the result of the passed in task if the
        // callback closure signature allows it.
        if constexpr (!std::is_same_v<ReturnType, void> &&
                      std::is_invocable_v<std::decay_t<CallbackClosure>,
                                          std::decay_t<ReturnType>>) {

            task_queue_.push(
                [task = std::forward<Callable>(task),
                 callback = std::forward<CallbackClosure>(callback),
                 ... task_args = std::forward<Args>(task_args)]() mutable {
                    std::invoke(std::forward<CallbackClosure>(callback),
                                std::invoke(std::forward<Callable>(task),
                                            std::forward<Args>(task_args)...));
                });

        } else { // Otherwise just invoke the callback after invoking the
                 // Callable.
            static_assert(std::is_invocable_v<std::decay_t<CallbackClosure>>,
                          "CallbackClosure must be invocable.");
            task_queue_.push(
                [task = std::forward<Callable>(task),
                 ... task_args = std::forward<Args>(task_args),
                 callback = std::forward<CallbackClosure>(callback)]() mutable {
                    std::invoke(std::forward<Callable>(task),
                                std::forward<Args>(task_args)...);
                    std::invoke(std::forward<CallbackClosure>(callback));
                });
        }
    }

  private:
    // Taking the stop_token by const& is fine here because jthread's
    // constructor returns a temporary by value so it gets copied anyways.
    void worker_task(const std::stop_token& stoken) {
        while (true) {
            auto task = task_queue_.pop(stoken);
            if (task) {
                task.value()();
            } else {
                break;
            }
        }
    }

    std::condition_variable_any queue_cv_;
    mutable std::mutex thread_pool_lock_;
    Container task_queue_;
    std::vector<std::jthread> threads_;
    size_t num_threads_;

    // atomic so it can be read outside of thread_pool_lock.
    std::atomic<bool> is_running_ = false;
}; // namespace pool_party

} // namespace pool_party

#endif // POOL_PARTY_HPP
