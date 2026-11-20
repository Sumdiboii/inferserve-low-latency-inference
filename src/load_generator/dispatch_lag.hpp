#pragma once

#include "open_loop_executor.hpp"

#include <cstddef>
#include <vector>

namespace inferserve::load_generator {

struct RequestDispatchLag {
    std::size_t request_index;
    double milliseconds;
};

struct DispatchLagStatistics {
    double mean_ms;
    double p50_ms;
    double p95_ms;
};

[[nodiscard]] std::vector<RequestDispatchLag> calculate_dispatch_lags(
    const std::vector<RequestTimestamps>& records);
[[nodiscard]] DispatchLagStatistics summarize_dispatch_lags(const std::vector<RequestDispatchLag>& dispatch_lags);

}  // namespace inferserve::load_generator
