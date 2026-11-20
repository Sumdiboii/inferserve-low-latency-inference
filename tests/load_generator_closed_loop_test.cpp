#include <gtest/gtest.h>

#include "closed_loop_executor.hpp"

#include <chrono>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using inferserve::load_generator::execute_closed_loop;
using inferserve::load_generator::RequestTimestamps;

TEST(ClosedLoopExecutor, SchedulesEachNextRequestAfterPreviousCompletion)
{
    using Clock = std::chrono::steady_clock;
    std::vector<Clock::time_point> callback_starts(3);
    std::vector<Clock::time_point> callback_finishes(3);

    const std::vector<RequestTimestamps> records = execute_closed_loop(3, [&](std::size_t index, double) {
        callback_starts[index] = Clock::now();
        std::this_thread::sleep_for(10ms);
        callback_finishes[index] = Clock::now();
    });

    ASSERT_EQ(records.size(), 3);
    for (std::size_t index = 0; index < records.size(); ++index) {
        EXPECT_EQ(records[index].request_index, index);
        EXPECT_LE(records[index].scheduled_arrival, records[index].actual_send);
        EXPECT_LE(records[index].actual_send, callback_starts[index]);
        EXPECT_LE(callback_finishes[index], records[index].completion);
        if (index > 0) {
            EXPECT_LE(records[index - 1].completion, records[index].scheduled_arrival);
            EXPECT_LE(records[index - 1].completion, records[index].actual_send);
        }
    }
}

}  // namespace
