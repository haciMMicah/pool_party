#include "BS_thread_pool.hpp"

#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <string_view>

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

using namespace BS;

namespace {

struct timer {
    time_point<high_resolution_clock> start_time;
    time_point<high_resolution_clock> stop_time;

    void start() { start_time = high_resolution_clock::now(); }
    void stop() { stop_time = high_resolution_clock::now(); }

    [[nodiscard]] nanoseconds duration() const {
        if (stop_time < start_time) {
            return 0ns;
        }
        return duration_cast<nanoseconds>(stop_time - start_time);
    }
};

[[maybe_unused]] nanoseconds
average_submit_detached_time(concurrency_t num_threads,
                             size_t num_submissions) {
    thread_pool pool{num_threads};
    // pool.pause();

    const auto empty_lambda = []() {};
    timer clock;
    clock.start();
    for (size_t i = 0; i < num_submissions; i++) {
        pool.detach_task(empty_lambda);
    }
    clock.stop();
    return clock.duration() / num_submissions;
}

//[[maybe_unused]] nanoseconds average_submit_time(size_t num_threads,
//                                                 size_t num_submissions,
//                                                 bool submit_after_started) {
//    thread_pool<BUFFER_SIZE, ALIGNMENT> pool{num_threads};
//    if (submit_after_started) {
//        pool.start();
//    }
//    const auto empty_lambda = []() {};
//    timer clock;
//    clock.start();
//    for (size_t i = 0; i < num_submissions; i++) {
//        [[maybe_unused]] auto fut = pool.submit(empty_lambda);
//    }
//    clock.stop();
//    return clock.duration() / num_submissions;
//}
//
//[[maybe_unused]] nanoseconds
// average_submit_with_callback_time(size_t num_threads, size_t num_submissions,
//                                  bool submit_after_started) {
//    thread_pool<BUFFER_SIZE, ALIGNMENT> pool{num_threads};
//    if (submit_after_started) {
//        pool.start();
//    }
//    const auto empty_lambda = [] {};
//    const auto empty_callback = [](std::exception_ptr) noexcept {};
//    timer clock;
//    clock.start();
//    for (size_t i = 0; i < num_submissions; i++) {
//        pool.submit_with_callback(empty_lambda, empty_callback);
//    }
//    clock.stop();
//    return clock.duration() / num_submissions;
//}

} // namespace

int main() {
    std::size_t num_submissions = 1'000'000;
    constexpr auto time_str = "{:20}, "
                              "{:3} threads, {:8} tasks, {:>12}"sv;
    const std::vector<std::tuple<
        std::string_view, std::function<nanoseconds(concurrency_t, size_t)>>>
        submit_functions = {
            //{"submit_detached"sv, average_submit_detached_time, true},
            {"submit_detached"sv, average_submit_detached_time},
            //{"submit"sv, average_submit_time, true},
            //{"submit"sv, average_submit_time, false},
            //{"submit_with_callback"sv, average_submit_with_callback_time,
            // true},
            //{"submit_with_callback"sv, average_submit_with_callback_time,
            // false},
        };

    for (const auto& [function_name, function] : submit_functions) {
        for (size_t num_threads = 1; num_threads <= 128;
             num_threads = num_threads << 1) {
            const auto submit_duration = function(num_threads, num_submissions);

            std::cout << std::format(time_str, function_name, num_threads,
                                     num_submissions, submit_duration)
                      << "\n";
        }
    }

    return 0;
}
