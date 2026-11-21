#include "closed_loop_executor.hpp"

#include <chrono>
#include <utility>

namespace inferserve::load_generator {

std::vector<RequestTimestamps> execute_closed_loop_with_results(
    std::size_t request_count,
    const std::function<RequestResult(std::size_t, double)>& execute_request)
{
    using Clock = std::chrono::steady_clock;
    const auto generator_start = Clock::now();
    std::vector<RequestTimestamps> records;
    records.reserve(request_count);

    for (std::size_t index = 0; index < request_count; ++index) {
        const auto scheduled_arrival = Clock::now();
        const double scheduled_seconds = std::chrono::duration<double>(scheduled_arrival - generator_start).count();
        const auto actual_send = Clock::now();
        RequestResult result = execute_request(index, scheduled_seconds);
        const auto completion = Clock::now();
        records.push_back({index, generator_start, scheduled_arrival, actual_send, completion, std::move(result)});
    }

    return records;
}

std::vector<RequestTimestamps> execute_closed_loop(
    std::size_t request_count,
    const std::function<void(std::size_t, double)>& execute_request)
{
    return execute_closed_loop_with_results(request_count, [&](std::size_t index, double scheduled_time) {
        execute_request(index, scheduled_time);
        return RequestResult{};
    });
}

}  // namespace inferserve::load_generator
