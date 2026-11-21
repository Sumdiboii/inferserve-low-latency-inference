#pragma once

#include "open_loop_executor.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace inferserve::load_generator {

struct LatencyPercentiles {
    std::size_t sample_count;
    double p50_ms;
    double p95_ms;
    double p99_ms;
};

struct RequestFailureCounts {
    std::size_t failed = 0;
    std::size_t rejected = 0;
    std::size_t undispatched = 0;
};

struct PerformanceStatistics {
    std::size_t scheduled_requests = 0;
    std::size_t completed_requests = 0;
    std::size_t successful_http_acknowledgements = 0;
    RequestFailureCounts failures;
    std::optional<LatencyPercentiles> latency_ms;
    std::optional<double> throughput_requests_per_second;
    std::optional<double> offered_requests_per_second;
    std::optional<double> http_acknowledgements_per_second;
};

// Offered rate uses the scheduled window from generator start through the last arrival,
// plus one mean inter-arrival interval to account for the final scheduled request.
[[nodiscard]] PerformanceStatistics calculate_performance_statistics(
    const std::vector<RequestTimestamps>& records,
    std::size_t warmup_count = 0);

}  // namespace inferserve::load_generator
