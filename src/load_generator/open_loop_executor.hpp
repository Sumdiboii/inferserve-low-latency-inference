#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace inferserve::load_generator {

struct RequestResult {
    // HTTP 202 confirms admission, while a terminal HTTP 200 confirms inference completion observed by the client.
    bool success = true;
    int http_status = 0;
    std::string error;
    bool dispatched = true;
    bool terminal_failure = false;
    bool http_acknowledged = false;
};

struct RequestTimestamps {
    std::size_t request_index;
    std::chrono::steady_clock::time_point generator_start;
    std::chrono::steady_clock::time_point scheduled_arrival;
    std::chrono::steady_clock::time_point actual_send;
    std::chrono::steady_clock::time_point completion;
    RequestResult result{};
};

[[nodiscard]] std::vector<RequestTimestamps> execute_open_loop(
    const std::vector<double>& scheduled_arrivals_seconds,
    const std::function<void(std::size_t, double)>& execute_request);
[[nodiscard]] std::vector<RequestTimestamps> execute_open_loop_with_results(
    const std::vector<double>& scheduled_arrivals_seconds,
    const std::function<RequestResult(std::size_t, double)>& execute_request);
}  // namespace inferserve::load_generator
