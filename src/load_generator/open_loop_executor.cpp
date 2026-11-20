#include "open_loop_executor.hpp"

#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace inferserve::load_generator {

std::vector<RequestTimestamps> execute_open_loop(
    const std::vector<double>& scheduled_arrivals_seconds,
    const std::function<void(std::size_t, double)>& execute_request)
{
    using Clock = std::chrono::steady_clock;
    const auto schedule_start = Clock::now();
    std::vector<std::future<RequestTimestamps>> executions;
    executions.reserve(scheduled_arrivals_seconds.size());

    for (std::size_t index = 0; index < scheduled_arrivals_seconds.size(); ++index) {
        const double scheduled_seconds = scheduled_arrivals_seconds[index];
        const auto offset = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(scheduled_seconds));
        const auto scheduled_arrival = schedule_start + offset;
        std::this_thread::sleep_until(scheduled_arrival);
        executions.push_back(std::async(std::launch::async,
                                        [&execute_request, index, scheduled_seconds, schedule_start, scheduled_arrival] {
                                            const auto actual_send = Clock::now();
                                            execute_request(index, scheduled_seconds);
                                            const auto completion = Clock::now();
                                            return RequestTimestamps{
                                                index, schedule_start, scheduled_arrival, actual_send, completion};
                                        }));
    }

    std::vector<RequestTimestamps> records;
    records.reserve(executions.size());
    for (auto& execution : executions) {
        records.push_back(execution.get());
    }
    return records;
}

}  // namespace inferserve::load_generator
