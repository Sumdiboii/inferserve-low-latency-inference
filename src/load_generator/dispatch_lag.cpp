#include "dispatch_lag.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace inferserve::load_generator {
namespace {

double nearest_rank_percentile(const std::vector<double>& sorted_values, double percentile)
{
    const auto rank = static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(sorted_values.size())));
    return sorted_values[rank - 1];
}

}  // namespace

std::vector<RequestDispatchLag> calculate_dispatch_lags(
    const std::vector<RequestTimestamps>& records,
    std::size_t warmup_count)
{
    std::vector<RequestDispatchLag> dispatch_lags;
    dispatch_lags.reserve(records.size() > warmup_count ? records.size() - warmup_count : 0);
    for (const auto& record : records) {
        if (record.request_index < warmup_count) {
            continue;
        }
        const auto lag = std::chrono::duration<double, std::milli>(record.actual_send - record.scheduled_arrival);
        dispatch_lags.push_back({record.request_index, lag.count()});
    }
    return dispatch_lags;
}

DispatchLagStatistics summarize_dispatch_lags(const std::vector<RequestDispatchLag>& dispatch_lags)
{
    if (dispatch_lags.empty()) {
        throw std::invalid_argument("cannot summarize empty dispatch-lag data");
    }

    std::vector<double> sorted_lags;
    sorted_lags.reserve(dispatch_lags.size());
    for (const auto& lag : dispatch_lags) {
        sorted_lags.push_back(lag.milliseconds);
    }
    std::sort(sorted_lags.begin(), sorted_lags.end());

    const double sum = std::accumulate(sorted_lags.begin(), sorted_lags.end(), 0.0);
    return {
        sum / static_cast<double>(sorted_lags.size()),
        nearest_rank_percentile(sorted_lags, 0.50),
        nearest_rank_percentile(sorted_lags, 0.95),
    };
}

}  // namespace inferserve::load_generator
