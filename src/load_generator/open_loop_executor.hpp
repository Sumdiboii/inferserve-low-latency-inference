#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

namespace inferserve::load_generator {

struct RequestTimestamps {
    std::size_t request_index;
    std::chrono::steady_clock::time_point generator_start;
    std::chrono::steady_clock::time_point scheduled_arrival;
    std::chrono::steady_clock::time_point actual_send;
    std::chrono::steady_clock::time_point completion;
};

[[nodiscard]] std::vector<RequestTimestamps> execute_open_loop(
    const std::vector<double>& scheduled_arrivals_seconds,
    const std::function<void(std::size_t, double)>& execute_request);

}  // namespace inferserve::load_generator
