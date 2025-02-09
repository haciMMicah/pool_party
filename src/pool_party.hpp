#ifndef POOL_PARTY_HPP
#define POOL_PARTY_HPP

#include <concepts>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include <iostream>

namespace pool_party {

using size_t = std::size_t;

// This class is needed to store generic callables that are not copyable due to
// move-only semantics for their captures. The class is a type-erased,
// movable-only generic callable that takes no arguments and returns void.
template <size_t BufferSize, size_t Alignment> class movable_callable {
  private:
    struct impl_base {
        virtual void call() = 0;
        virtual void move(impl_base* address) = 0;
        virtual ~impl_base() = default;

        impl_base() = default;
        impl_base(const impl_base&) = delete;
        impl_base(impl_base&&) = default;
        impl_base& operator=(const impl_base&) = delete;
        impl_base& operator=(impl_base&&) = default;
    };

    template <typename Callable> struct impl_derived : impl_base {
        Callable func_;
        impl_derived(Callable&& func) : func_(std::move(func)) {}
        void call() override { std::invoke(func_); }
        void move(impl_base* address) override {
            ::new (address) impl_derived<Callable>(std::move(*this));
        }
    };

    alignas(Alignment) std::array<std::byte, BufferSize> buffer;
    bool use_heap = false;
    impl_base* pimpl_ = nullptr;

  public:
    template <typename Callable> movable_callable(Callable&& function) {
        using Derived = impl_derived<Callable>;
        if constexpr (sizeof(Derived) <= BufferSize) {
            // NOLINTNEXTLINE: Allow reinterpret_cast for SBO
            pimpl_ = reinterpret_cast<impl_base*>(buffer.data());
            ::new (pimpl_) Derived(std::move(function));
            use_heap = false;
        } else {
            use_heap = true;
            // NOLINTNEXTLINE: Allow raw new for SBO
            pimpl_ = new Derived(std::move(function));
        }
    }

    ~movable_callable() {
        if (!use_heap) {
            pimpl_->~impl_base();
        } else {
            delete pimpl_;
        }
        pimpl_ = nullptr;
    }
    movable_callable() = default;
    movable_callable(const movable_callable&) = delete;
    movable_callable(movable_callable&) = delete;
    movable_callable(movable_callable&& other) noexcept {
        if (!other.use_heap) {
            // NOLINTNEXTLINE: Allow reinterpret_cast for SBO
            pimpl_ = reinterpret_cast<impl_base*>(buffer.data());
            other.pimpl_->move(pimpl_);
        } else {
            use_heap = true;
            std::swap(pimpl_, other.pimpl_);
        }
    }
    movable_callable& operator=(movable_callable&& other) noexcept {
        buffer.swap(other.buffer);
        std::swap(pimpl_, other.pimpl_);
        std::swap(use_heap, other.use_heap);
        if (!use_heap) {
            // NOLINTNEXTLINE: Allow reinterpret_cast for SBO
            pimpl_ = reinterpret_cast<impl_base*>(buffer.data());
        }
        if (!other.use_heap) {
            // NOLINTNEXTLINE: Allow reinterpret_cast for SBO
            other.pimpl_ = reinterpret_cast<impl_base*>(other.buffer.data());
        }
        return *this;
    }
    movable_callable& operator=(const movable_callable&) = delete;

    void operator()() { pimpl_->call(); }
}; // class movable_callable

template <typename T>
concept not_copyable = not std::copyable<T>;

template <typename F, typename... Args>
concept move_only_invocable =
    std::invocable<F, Args...> && std::movable<F> && not_copyable<F>;

template <typename Queue>
concept thread_pool_queue =
    requires { typename Queue::value_type; } &&
    requires(Queue queue, const Queue c_queue,
             typename Queue::value_type&& value) {
        { queue.push(std::move(value)) } -> std::same_as<void>;
        {
            queue.pop(std::stop_token())
        } -> std::same_as<std::optional<typename Queue::value_type>>;
        { c_queue.size() } -> std::convertible_to<size_t>;
        { c_queue.empty() } -> std::same_as<bool>;
        { queue.start() } -> std::same_as<void>;
        { queue.stop() } -> std::same_as<void>;
    };

template <typename T> class simple_thread_safe_queue {
  public:
    using value_type = T;
    void stop() {
        {
            std::lock_guard lock(queue_lock_);
            stopping_.store(true);
        }
        queue_cv_.notify_all();
    }
    void start() {
        std::lock_guard lock(queue_lock_);
        stopping_ = false;
    }
    void push(value_type&& value) {
        std::unique_lock lock(queue_lock_);
        queue_.push(std::forward<value_type>(value));
        lock.unlock();
        queue_cv_.notify_one();
    }

    std::optional<value_type> pop(std::stop_token) {
        std::unique_lock lock(queue_lock_);
        // queue_cv_.wait(lock, stoken, [this] { return !queue_.empty(); });
        queue_cv_.wait(lock,
                       [this] { return stopping_.load() || !queue_.empty(); });
        if (stopping_) {
            return std::nullopt;
        }

        // if (stoken.stop_requested()) {
        //     return std::nullopt;
        // }

        auto front = std::move(queue_.front());
        queue_.pop();
        return front;
    }

    size_t size() const {
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
    std::condition_variable queue_cv_;
    std::atomic<bool> stopping_ = false;
};

template <size_t BufferSize = 64UL, size_t Alignment = 8UL,
          move_only_invocable CallableWrapper =
              movable_callable<BufferSize, Alignment>,
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
        // Double check lock to avoid needing to lock if the threads are
        // already running.
        if (is_running_) {
            return;
        }

        std::unique_lock pool_lock(thread_pool_lock_);
        if (is_running_) {
            return;
        }

        threads_ = std::vector<std::jthread>(num_threads_);
        task_queue_.start();
        for (size_t i = 0; i < num_threads_; i++) {
            threads_[i] =
                std::jthread{std::bind_front(&thread_pool::worker_task, this)};
        }
        is_running_ = true;
    }

    void stop() {
        // Double check lock to avoid needing to lock if the threads are
        // already stopped.
        if (!is_running_) {
            return;
        }
        std::unique_lock pool_lock(thread_pool_lock_);

        if (!is_running_) {
            return;
        }

        task_queue_.stop();
        for (auto& thread : threads_) {
            // thread.request_stop();
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
                // TODO: Maybe call a user set callback to foward the
                // exception.
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
        requires std::is_default_constructible_v<ReturnType> or
                 std::is_same_v<ReturnType, void>
    void submit_with_callback(Callable&& task, CallbackClosure&& callback,
                              Args&&... task_args) {
        // Call callback with the result of the passed in task if the
        // callback closure signature allows it.
        if constexpr (!std::is_same_v<ReturnType, void> &&
                      std::is_nothrow_invocable_v<
                          CallbackClosure, std::exception_ptr, ReturnType>) {

            task_queue_.push(
                [task = std::forward<Callable>(task),
                 callback = std::forward<CallbackClosure>(callback),
                 ... task_args = std::forward<Args>(task_args)]() mutable {
                    try {
                        auto result =
                            std::invoke(std::forward<Callable>(task),
                                        std::forward<Args>(task_args)...);
                        std::invoke(std::forward<CallbackClosure>(callback),
                                    std::exception_ptr{}, result);
                    } catch (...) {
                        std::invoke(std::forward<CallbackClosure>(callback),
                                    std::current_exception(), ReturnType{});
                    }
                });

        } else { // Otherwise just invoke the callback after invoking the
                 // Callable.
            static_assert(
                std::is_nothrow_invocable_v<std::decay_t<CallbackClosure>,
                                            std::exception_ptr>,
                "CallbackClosure must be nothrow invocable.");
            task_queue_.push(
                [task = std::forward<Callable>(task),
                 ... task_args = std::forward<Args>(task_args),
                 callback = std::forward<CallbackClosure>(callback)]() mutable {
                    try {
                        std::invoke(std::forward<Callable>(task),
                                    std::forward<Args>(task_args)...);
                        std::invoke(std::forward<CallbackClosure>(callback),
                                    std::exception_ptr{});
                    } catch (...) {
                        std::invoke(std::forward<CallbackClosure>(callback),
                                    std::current_exception());
                    }
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

    mutable std::mutex thread_pool_lock_;
    Container task_queue_;
    std::vector<std::jthread> threads_;
    size_t num_threads_;

    // atomic so it can be read outside of thread_pool_lock.
    std::atomic<bool> is_running_ = false;
}; // class thread_pool

} // namespace pool_party

#endif // POOL_PARTY_HPP
