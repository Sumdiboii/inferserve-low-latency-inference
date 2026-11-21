#include "performance_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace inferserve::load_generator {
namespace {

double nearest_rank_percentile(const std::vector<double>& sorted_values, double percentile)
{
    const auto rank = static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(sorted_values.size())));
    return sorted_values[rank - 1];
}

bool has_http_result(const RequestResult& result)
{
    return result.http_status != 0 || !result.error.empty();
}

bool is_successful_http_acknowledgement(const RequestResult& result)
{
    return result.success && result.http_status >= 200 && result.http_status < 300;
}

}  // namespace

PerformanceStatistics calculate_performance_statistics(const std::vector<RequestTimestamps>& records)
{
    PerformanceStatistics statistics;
    statistics.scheduled_requests = records.size();
    if (records.empty()) {
        return statistics;
    }

    using Clock = std::chrono::steady_clock;
    Clock::time_point measurement_start = records.front().generator_start;
    Clock::time_point earliest_scheduled = records.front().scheduled_arrival;
    Clock::time_point latest_scheduled = records.front().scheduled_arrival;
    Clock::time_point latest_completion = records.front().generator_start;
    bool has_completion = false;
    bool has_http_attempt = false;
    std::vector<double> latency_samples_ms;
    latency_samples_ms.reserve(records.size());

    for (const RequestTimestamps& record : records) {
        measurement_start = std::min(measurement_start, record.generator_start);
        earliest_scheduled = std::min(earliest_scheduled, record.scheduled_arrival);
        latest_scheduled = std::max(latest_scheduled, record.scheduled_arrival);

        if (!record.result.dispatched) {
            ++statistics.failures.undispatched;
            continue;
        }

        if (record.completion >= record.actual_send) {
            ++statistics.completed_requests;
            latest_completion = has_completion ? std::max(latest_completion, record.completion) : record.completion;
            has_completion = true;
        }
        if (record.completion >= record.scheduled_arrival) {
            latency_samples_ms.push_back(
                std::chrono::duration<double, std::milli>(record.completion - record.scheduled_arrival).count());
        }

        has_http_attempt = has_http_attempt || has_http_result(record.result);
        if (is_successful_http_acknowledgement(record.result)) {
            ++statistics.successful_http_acknowledgements;
        } else if (!record.result.success) {
            if (record.result.http_status != 0) {
                ++statistics.failures.rejected;
            } else {
                ++statistics.failures.failed;
            }
        }
    }

    if (!latency_samples_ms.empty()) {
        std::sort(latency_samples_ms.begin(), latency_samples_ms.end());
        statistics.latency_ms = LatencyPercentiles{
            latency_samples_ms.size(),
            nearest_rank_percentile(latency_samples_ms, 0.50),
            nearest_rank_percentile(latency_samples_ms, 0.95),
            nearest_rank_percentile(latency_samples_ms, 0.99),
        };
    }

    if (records.size() >= 2) {
        const auto schedule_span = latest_scheduled - measurement_start;
        const auto arrival_span = latest_scheduled - earliest_scheduled;
        const auto average_interval = arrival_span / static_cast<Clock::duration::rep>(records.size() - 1);
        const auto offered_window = schedule_span + average_interval;
        if (offered_window > Clock::duration::zero()) {
            const double seconds = std::chrono::duration<double>(offered_window).count();
            statistics.offered_requests_per_second = static_cast<double>(records.size()) / seconds;
        }
    }

    if (has_completion) {
        const auto elapsed = latest_completion - measurement_start;
        if (elapsed > Clock::duration::zero()) {
            const double seconds = std::chrono::duration<double>(elapsed).count();
            statistics.throughput_requests_per_second =
                static_cast<double>(statistics.completed_requests) / seconds;
            if (has_http_attempt) {
                statistics.http_acknowledgements_per_second =
                    static_cast<double>(statistics.successful_http_acknowledgements) / seconds;
            }
        }
    }

    return statistics;
}

}  // namespace inferserve::load_generator
