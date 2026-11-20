#include <gtest/gtest.h>

#include "dispatch_lag.hpp"

#include <chrono>
#include <vector>

namespace {

using namespace std::chrono_literals;
using inferserve::load_generator::calculate_dispatch_lags;
using inferserve::load_generator::summarize_dispatch_lags;
using inferserve::load_generator::RequestTimestamps;

TEST(DispatchLag, CalculatesPerRequestLagAndNearestRankStatistics)
{
    using Clock = std::chrono::steady_clock;
    const auto origin = Clock::time_point{};
    const std::vector<RequestTimestamps> records{
        {42, origin, origin, origin + 40ms, origin + 2h},
        {7, origin, origin + 1s, origin + 1s + 10ms, origin + 3h},
        {88, origin, origin + 2s, origin + 2s + 30ms, origin + 4h},
        {5, origin, origin + 3s, origin + 3s + 20ms, origin + 5h},
    };

    const auto dispatch_lags = calculate_dispatch_lags(records);

    ASSERT_EQ(dispatch_lags.size(), 4);
    EXPECT_EQ(dispatch_lags[0].request_index, 42);
    EXPECT_DOUBLE_EQ(dispatch_lags[0].milliseconds, 40.0);
    EXPECT_EQ(dispatch_lags[1].request_index, 7);
    EXPECT_DOUBLE_EQ(dispatch_lags[1].milliseconds, 10.0);
    EXPECT_EQ(dispatch_lags[2].request_index, 88);
    EXPECT_DOUBLE_EQ(dispatch_lags[2].milliseconds, 30.0);
    EXPECT_EQ(dispatch_lags[3].request_index, 5);
    EXPECT_DOUBLE_EQ(dispatch_lags[3].milliseconds, 20.0);

    const auto statistics = summarize_dispatch_lags(dispatch_lags);
    EXPECT_DOUBLE_EQ(statistics.mean_ms, 25.0);
    EXPECT_DOUBLE_EQ(statistics.p50_ms, 20.0);
    EXPECT_DOUBLE_EQ(statistics.p95_ms, 40.0);
}

}  // namespace
