#include <gtest/gtest.h>

#include "performance_stats.hpp"

#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using inferserve::load_generator::PerformanceStatistics;
using inferserve::load_generator::RequestResult;
using inferserve::load_generator::RequestTimestamps;
using inferserve::load_generator::calculate_performance_statistics;

RequestTimestamps record(std::size_t index,
                         std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point scheduled,
                         std::chrono::steady_clock::time_point sent,
                         std::chrono::steady_clock::time_point completed,
                         RequestResult result = {})
{
    return {index, start, scheduled, sent, completed, std::move(result)};
}

TEST(PerformanceStatistics, UsesNearestRankPercentilesFromScheduledArrival)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::vector<RequestTimestamps> records{
        record(0, origin, origin, origin + 5ms, origin + 10ms),
        record(1, origin, origin + 1s, origin + 1s + 10ms, origin + 1s + 20ms),
        record(2, origin, origin + 2s, origin + 2s + 15ms, origin + 2s + 30ms),
        record(3, origin, origin + 3s, origin + 3s + 20ms, origin + 3s + 40ms),
        record(4, origin, origin + 4s, origin + 4s + 50ms, origin + 4s + 100ms),
    };

    const PerformanceStatistics statistics = calculate_performance_statistics(records);

    ASSERT_TRUE(statistics.latency_ms.has_value());
    EXPECT_EQ(statistics.latency_ms->sample_count, 5U);
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p50_ms, 30.0);
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p95_ms, 100.0);
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p99_ms, 100.0);
}

TEST(PerformanceStatistics, HandlesSingleSamplePercentilesAndUndefinedOfferedRate)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const PerformanceStatistics statistics = calculate_performance_statistics(
        {record(0, origin, origin, origin, origin + 37ms)});

    ASSERT_TRUE(statistics.latency_ms.has_value());
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p50_ms, 37.0);
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p95_ms, 37.0);
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p99_ms, 37.0);
    EXPECT_FALSE(statistics.offered_requests_per_second.has_value());
    EXPECT_FALSE(statistics.http_acknowledgements_per_second.has_value());
}

TEST(PerformanceStatistics, SeparatesOfferedThroughputAcknowledgementAndFailureRates)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::vector<RequestTimestamps> records{
        record(0, origin, origin, origin + 400ms, origin + 500ms, RequestResult{true, 202, {}}),
        record(1, origin, origin + 1s, origin + 1400ms, origin + 1500ms,
               RequestResult{false, 503, "request rejected"}),
        record(2, origin, origin + 2s, origin + 2400ms, origin + 2500ms,
               RequestResult{false, 0, "connection failed"}),
        record(3, origin, origin + 3s, origin + 3100ms, origin + 3100ms,
               RequestResult{false, 0, "request not dispatched: pending queue is full", false}),
    };

    const PerformanceStatistics statistics = calculate_performance_statistics(records);

    EXPECT_EQ(statistics.scheduled_requests, 4U);
    EXPECT_EQ(statistics.completed_requests, 1U);
    EXPECT_EQ(statistics.successful_http_acknowledgements, 1U);
    EXPECT_EQ(statistics.failures.failed, 1U);
    EXPECT_EQ(statistics.failures.rejected, 1U);
    EXPECT_EQ(statistics.failures.undispatched, 1U);
    ASSERT_TRUE(statistics.offered_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.offered_requests_per_second, 1.0, 1e-9);
    ASSERT_TRUE(statistics.throughput_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.throughput_requests_per_second, 0.4, 1e-9);
    ASSERT_TRUE(statistics.http_acknowledgements_per_second.has_value());
    EXPECT_NEAR(*statistics.http_acknowledgements_per_second, 0.4, 1e-9);
}

TEST(PerformanceStatistics, EmptySamplesProduceNoRatesOrPercentiles)
{
    const PerformanceStatistics statistics = calculate_performance_statistics({});

    EXPECT_EQ(statistics.scheduled_requests, 0U);
    EXPECT_EQ(statistics.completed_requests, 0U);
    EXPECT_FALSE(statistics.latency_ms.has_value());
    EXPECT_FALSE(statistics.throughput_requests_per_second.has_value());
    EXPECT_FALSE(statistics.offered_requests_per_second.has_value());
    EXPECT_FALSE(statistics.http_acknowledgements_per_second.has_value());
}

TEST(PerformanceStatistics, ExcludesWarmupFromStatisticsAndMeasuredWindows)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::vector<RequestTimestamps> records{
        record(0, origin, origin, origin + 10ms, origin + 100ms,
               RequestResult{false, 503, "warmup rejected"}),
        record(1, origin, origin + 100ms, origin + 110ms, origin + 200ms,
               RequestResult{false, 0, "warmup failed"}),
        record(2, origin, origin + 1s, origin + 1010ms, origin + 1020ms,
               RequestResult{true, 202, {}}),
        record(3, origin, origin + 2s, origin + 2010ms, origin + 2020ms,
               RequestResult{true, 200, {}}),
    };

    const PerformanceStatistics statistics = calculate_performance_statistics(records, 2);

    EXPECT_EQ(statistics.scheduled_requests, 2U);
    EXPECT_EQ(statistics.completed_requests, 2U);
    EXPECT_EQ(statistics.successful_http_acknowledgements, 1U);
    EXPECT_EQ(statistics.failures.failed, 0U);
    EXPECT_EQ(statistics.failures.rejected, 0U);
    ASSERT_TRUE(statistics.latency_ms.has_value());
    EXPECT_EQ(statistics.latency_ms->sample_count, 2U);
    EXPECT_DOUBLE_EQ(statistics.latency_ms->p50_ms, 20.0);
    ASSERT_TRUE(statistics.throughput_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.throughput_requests_per_second, 2.0 / 1.020, 1e-9);
    ASSERT_TRUE(statistics.offered_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.offered_requests_per_second, 1.0, 1e-9);
}

TEST(PerformanceStatistics, ZeroWarmupPreservesOriginalMeasurementWindows)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::vector<RequestTimestamps> records{
        record(0, origin, origin + 100ms, origin + 110ms, origin + 500ms),
        record(1, origin, origin + 1100ms, origin + 1110ms, origin + 1500ms),
    };

    const PerformanceStatistics statistics = calculate_performance_statistics(records, 0);

    ASSERT_TRUE(statistics.throughput_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.throughput_requests_per_second, 2.0 / 1.5, 1e-9);
    ASSERT_TRUE(statistics.offered_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.offered_requests_per_second, 2.0 / 2.1, 1e-9);
}

TEST(PerformanceStatistics, OpenLoopWarmupWindowStartsAtFirstMeasuredScheduledArrival)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::vector<RequestTimestamps> records{
        record(0, origin, origin, origin + 10ms, origin + 100ms),
        record(1, origin, origin + 500ms, origin + 510ms, origin + 600ms),
        record(2, origin, origin + 1000ms, origin + 1010ms, origin + 1100ms),
        record(3, origin, origin + 1500ms, origin + 1510ms, origin + 1600ms),
    };

    const PerformanceStatistics statistics = calculate_performance_statistics(records, 2);

    ASSERT_TRUE(statistics.throughput_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.throughput_requests_per_second, 2.0 / 0.600, 1e-9);
    ASSERT_TRUE(statistics.offered_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.offered_requests_per_second, 2.0 / 1.0, 1e-9);
}

TEST(PerformanceStatistics, ClosedLoopWarmupWindowStartsAtFirstMeasuredArrival)
{
    const auto origin = std::chrono::steady_clock::time_point{};
    const std::vector<RequestTimestamps> records{
        record(0, origin, origin, origin + 10ms, origin + 100ms),
        record(1, origin, origin + 100ms, origin + 110ms, origin + 200ms),
        record(2, origin, origin + 300ms, origin + 310ms, origin + 400ms),
        record(3, origin, origin + 500ms, origin + 510ms, origin + 600ms),
    };

    const PerformanceStatistics statistics = calculate_performance_statistics(records, 2);

    ASSERT_TRUE(statistics.throughput_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.throughput_requests_per_second, 2.0 / 0.300, 1e-9);
    ASSERT_TRUE(statistics.offered_requests_per_second.has_value());
    EXPECT_NEAR(*statistics.offered_requests_per_second, 2.0 / 0.400, 1e-9);
}

}  // namespace
