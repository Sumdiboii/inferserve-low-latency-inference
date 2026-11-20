#include <gtest/gtest.h>

#include "traffic_pattern.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using inferserve::load_generator::burst_arrivals;
using inferserve::load_generator::fixed_rate_arrivals;
using inferserve::load_generator::poisson_arrivals;

TEST(TrafficPattern, FixedRateSchedulesRegularIntervalsStartingAtZero)
{
    const auto arrivals = fixed_rate_arrivals(4, 2.0);

    ASSERT_EQ(arrivals.size(), 4);
    EXPECT_DOUBLE_EQ(arrivals[0], 0.0);
    EXPECT_DOUBLE_EQ(arrivals[1], 0.5);
    EXPECT_DOUBLE_EQ(arrivals[2], 1.0);
    EXPECT_DOUBLE_EQ(arrivals[3], 1.5);
}

TEST(TrafficPattern, BurstSchedulesEveryArrivalAtTheSameTime)
{
    EXPECT_EQ(burst_arrivals(3), (std::vector<double>{0.0, 0.0, 0.0}));
}

TEST(TrafficPattern, SeededPoissonScheduleIsRepeatableAndIncreasing)
{
    const auto first = poisson_arrivals(8, 3.5, 42);
    const auto repeated = poisson_arrivals(8, 3.5, 42);
    const auto different_seed = poisson_arrivals(8, 3.5, 43);

    EXPECT_EQ(first, repeated);
    EXPECT_NE(first, different_seed);
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_TRUE(std::isfinite(first[index]));
        EXPECT_GT(first[index], 0.0);
        if (index > 0) {
            EXPECT_GT(first[index], first[index - 1]);
        }
    }
}

TEST(TrafficPattern, RateMustBeFiniteAndPositive)
{
    EXPECT_THROW(static_cast<void>(fixed_rate_arrivals(1, 0.0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(poisson_arrivals(1, -1.0, 42)), std::invalid_argument);
}

}  // namespace
