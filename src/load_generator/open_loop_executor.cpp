#include "open_loop_executor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace inferserve::load_generator {
namespace {

constexpr std::size_t kOpenLoopWorkerCount = 4;

struct ScheduledRequest {
    std::size_t index;
    double scheduled_seconds;
    std::chrono::steady_clock::time_point scheduled_arrival;
};

}  // namespace

std::vector<RequestTimestamps> execute_open_loop(
    const std::vector<double>& scheduled_arrivals_seconds,
    const std::function<void(std::size_t, double)>& execute_request)
{
    using Clock = std::chrono::steady_clock;
    const auto schedule_start = Clock::now();
    std::vector<RequestTimestamps> records(scheduled_arrivals_seconds.size());
    std::vector<std::exception_ptr> callback_errors(scheduled_arrivals_seconds.size());
    std::deque<ScheduledRequest> pending_requests;
    std::mutex queue_mutex;
    std::condition_variable request_available;
    bool scheduling_complete = false;

    const std::size_t worker_count = std::min(kOpenLoopWorkerCount, scheduled_arrivals_seconds.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    try {
        for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
            workers.emplace_back([&] {
                while (true) {
                    ScheduledRequest request{};
                    {
                        std::unique_lock lock(queue_mutex);
                        request_available.wait(lock, [&] {
                            return scheduling_complete || !pending_requests.empty();
                        });
                        if (pending_requests.empty()) {
                            return;
                        }
                        request = pending_requests.front();
                        pending_requests.pop_front();
                    }

                    const auto actual_send = Clock::now();
                    try {
                        execute_request(request.index, request.scheduled_seconds);
                    } catch (...) {
                        callback_errors[request.index] = std::current_exception();
                    }
                    const auto completion = Clock::now();
                    records[request.index] = RequestTimestamps{
                        request.index, schedule_start, request.scheduled_arrival, actual_send, completion};
                }
            });
        }
    } catch (...) {
        {
            std::lock_guard lock(queue_mutex);
            scheduling_complete = true;
        }
        request_available.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }

    std::exception_ptr scheduling_error;
    try {
        for (std::size_t index = 0; index < scheduled_arrivals_seconds.size(); ++index) {
            const double scheduled_seconds = scheduled_arrivals_seconds[index];
            const auto offset = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(scheduled_seconds));
            const auto scheduled_arrival = schedule_start + offset;
            std::this_thread::sleep_until(scheduled_arrival);
            {
                std::lock_guard lock(queue_mutex);
                pending_requests.push_back({index, scheduled_seconds, scheduled_arrival});
            }
            request_available.notify_one();
        }
    } catch (...) {
        scheduling_error = std::current_exception();
    }

    {
        std::lock_guard lock(queue_mutex);
        scheduling_complete = true;
    }
    request_available.notify_all();
    for (std::thread& worker : workers) {
        worker.join();
    }

    if (scheduling_error) {
        std::rethrow_exception(scheduling_error);
    }
    for (const std::exception_ptr& callback_error : callback_errors) {
        if (callback_error) {
            std::rethrow_exception(callback_error);
        }
    }
    return records;
}

}  // namespace inferserve::load_generator
