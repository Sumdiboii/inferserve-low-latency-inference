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
    return result.http_acknowledged ||
        (result.success && result.http_status == 202);
}

}  // namespace

PerformanceStatistics calculate_performance_statistics(
    const std::vector<RequestTimestamps>& records,
    std::size_t warmup_count)
{
    PerformanceStatistics statistics;
    if (warmup_count > records.size()) {
        warmup_count = records.size();
    }
    statistics.scheduled_requests = records.size() - warmup_count;
    if (statistics.scheduled_requests == 0) {
        return statistics;
    }

    using Clock = std::chrono::steady_clock;
    Clock::time_point measurement_start = warmup_count == 0
                                              ? records.front().generator_start
                                              : Clock::time_point{};
    Clock::time_point earliest_scheduled = warmup_count == 0
                                               ? records.front().scheduled_arrival
                                               : Clock::time_point{};
    Clock::time_point latest_scheduled = warmup_count == 0
                                             ? records.front().scheduled_arrival
                                             : Clock::time_point{};
    Clock::time_point latest_completion = records.front().generator_start;
    bool has_completion = false;
    bool has_http_attempt = false;
    std::vector<double> latency_samples_ms;
    latency_samples_ms.reserve(statistics.scheduled_requests);
    bool has_measured_request = false;

    for (const RequestTimestamps& record : records) {
        if (record.request_index < warmup_count) {
            continue;
        }
        if (!has_measured_request) {
            if (warmup_count > 0) {
                measurement_start = record.scheduled_arrival;
                earliest_scheduled = record.scheduled_arrival;
                latest_scheduled = record.scheduled_arrival;
            }
            has_measured_request = true;
        }
        earliest_scheduled = std::min(earliest_scheduled, record.scheduled_arrival);
        latest_scheduled = std::max(latest_scheduled, record.scheduled_arrival);

        if (!record.result.dispatched) {
            ++statistics.failures.undispatched;
            continue;
        }

        if (record.result.success && record.completion >= record.actual_send) {
            ++statistics.completed_requests;
            latest_completion = has_completion ? std::max(latest_completion, record.completion) : record.completion;
            has_completion = true;
            if (record.completion >= record.scheduled_arrival) {
                latency_samples_ms.push_back(
                    std::chrono::duration<double, std::milli>(record.completion - record.scheduled_arrival).count());
            }
        }

        has_http_attempt = has_http_attempt || has_http_result(record.result);
        if (is_successful_http_acknowledgement(record.result)) {
            ++statistics.successful_http_acknowledgements;
        }
        if (!record.result.success) {
            if (record.result.terminal_failure) {
                ++statistics.failures.terminal_failures;
            } else if (record.result.http_status != 0) {
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

    if (statistics.scheduled_requests >= 2) {
        const auto schedule_span = latest_scheduled - measurement_start;
        const auto arrival_span = latest_scheduled - earliest_scheduled;
        const auto average_interval =
            arrival_span / static_cast<Clock::duration::rep>(statistics.scheduled_requests - 1);
        const auto offered_window = schedule_span + average_interval;
        if (offered_window > Clock::duration::zero()) {
            const double seconds = std::chrono::duration<double>(offered_window).count();
            statistics.offered_requests_per_second =
                static_cast<double>(statistics.scheduled_requests) / seconds;
        }
    }

    if (has_completion) {
        const auto elapsed = warmup_count == 0
                                 ? latest_completion - measurement_start
                                 : latest_completion - earliest_scheduled;
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
