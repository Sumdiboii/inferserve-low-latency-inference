#include <gtest/gtest.h>

#include "open_loop_executor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using inferserve::load_generator::execute_open_loop;
using inferserve::load_generator::execute_open_loop_with_results;
using inferserve::load_generator::RequestTimestamps;

TEST(OpenLoopExecutor, LaterArrivalStartsWhileEarlierExecutionIsStillBlocked)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool first_started = false;
    bool release_first = false;
    bool second_started_before_release = false;

    std::thread scheduler([&] {
        static_cast<void>(execute_open_loop({0.0, 0.01}, [&](std::size_t index, double) {
            std::unique_lock lock(mutex);
            if (index == 0) {
                first_started = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_first; });
            } else {
                second_started_before_release = !release_first;
                condition.notify_all();
            }
        }));
    });

    {
        std::unique_lock lock(mutex);
        const bool first_arrived = condition.wait_for(lock, 1s, [&] { return first_started; });
        if (first_arrived) {
            condition.wait_for(lock, 1s, [&] { return second_started_before_release; });
        }
        release_first = true;
        condition.notify_all();
    }
    scheduler.join();

    EXPECT_TRUE(first_started);
    EXPECT_TRUE(second_started_before_release);
}

TEST(OpenLoopExecutor, UsesBoundedWorkersForManySimultaneousArrivals)
{
    constexpr std::size_t request_count = 32;
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t active_callbacks = 0;
    std::size_t maximum_active_callbacks = 0;
    bool release_callbacks = false;
    std::set<std::thread::id> callback_threads;
    std::vector<double> arrivals(request_count, 0.0);

    std::thread scheduler([&] {
        static_cast<void>(execute_open_loop(arrivals, [&](std::size_t, double) {
            std::unique_lock lock(mutex);
            ++active_callbacks;
            maximum_active_callbacks = std::max(maximum_active_callbacks, active_callbacks);
            callback_threads.insert(std::this_thread::get_id());
            condition.notify_all();
            condition.wait(lock, [&] { return release_callbacks; });
            --active_callbacks;
        }));
    });

    bool worker_limit_reached = false;
    {
        std::unique_lock lock(mutex);
        worker_limit_reached = condition.wait_for(lock, 1s, [&] { return active_callbacks >= 4; });
        release_callbacks = true;
        condition.notify_all();
    }
    scheduler.join();

    EXPECT_TRUE(worker_limit_reached);
    EXPECT_EQ(maximum_active_callbacks, 4U);
    EXPECT_EQ(callback_threads.size(), 4U);
}

TEST(OpenLoopExecutor, BoundsPendingQueueAndRecordsUndispatchedArrivals)
{
    constexpr std::size_t request_count = 1200;
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t active_callbacks = 0;
    bool release_callbacks = false;
    std::vector<double> arrivals(request_count, 0.0);
    std::vector<inferserve::load_generator::RequestTimestamps> records;

    std::thread scheduler([&] {
        records = execute_open_loop_with_results(arrivals, [&](std::size_t, double) {
            std::unique_lock lock(mutex);
            ++active_callbacks;
            condition.notify_all();
            condition.wait(lock, [&] { return release_callbacks; });
            return inferserve::load_generator::RequestResult{};
        });
    });

    bool all_workers_started = false;
    {
        std::unique_lock lock(mutex);
        all_workers_started = condition.wait_for(lock, std::chrono::seconds(1), [&] { return active_callbacks == 4; });
        if (all_workers_started) {
            condition.wait_for(lock, std::chrono::milliseconds(100), [] { return false; });
        }
        release_callbacks = true;
        condition.notify_all();
    }
    scheduler.join();

    ASSERT_TRUE(all_workers_started);
    ASSERT_EQ(records.size(), request_count);
    std::size_t undispatched_count = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        EXPECT_EQ(records[index].request_index, index);
        EXPECT_LE(records[index].scheduled_arrival, records[index].actual_send);
        EXPECT_LE(records[index].actual_send, records[index].completion);
        if (!records[index].result.success) {
            ++undispatched_count;
            EXPECT_NE(records[index].result.error.find("pending queue is full"), std::string::npos);
        }
    }
    EXPECT_GT(undispatched_count, 0U);
}

TEST(OpenLoopExecutor, RecordsOrderedRequestTimestampsAroundCallbackExecution)
{
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::condition_variable condition;
    bool first_started = false;
    bool release_first = false;
    bool second_finished = false;
    std::vector<Clock::time_point> callback_starts(2);
    std::vector<Clock::time_point> callback_finishes(2);
    std::vector<RequestTimestamps> records;

    std::thread scheduler([&] {
        records = execute_open_loop({0.0, 0.01}, [&](std::size_t index, double) {
            callback_starts[index] = Clock::now();
            if (index == 0) {
                std::unique_lock lock(mutex);
                first_started = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_first; });
            } else {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return first_started; });
                second_finished = true;
                condition.notify_all();
            }
            callback_finishes[index] = Clock::now();
        });
    });

    bool first_arrived = false;
    bool second_arrived = false;
    {
        std::unique_lock lock(mutex);
        first_arrived = condition.wait_for(lock, 1s, [&] { return first_started; });
        if (first_arrived) {
            second_arrived = condition.wait_for(lock, 1s, [&] { return second_finished; });
        }
        release_first = true;
        condition.notify_all();
    }
    scheduler.join();

    ASSERT_TRUE(first_arrived);
    ASSERT_TRUE(second_arrived);
    ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(records[0].request_index, 0);
    EXPECT_EQ(records[1].request_index, 1);
    EXPECT_LT(records[0].scheduled_arrival, records[1].scheduled_arrival);
    EXPECT_LE(records[0].scheduled_arrival, records[0].actual_send);
    EXPECT_LE(records[1].scheduled_arrival, records[1].actual_send);
    EXPECT_LE(records[0].actual_send, callback_starts[0]);
    EXPECT_LE(records[1].actual_send, callback_starts[1]);
    EXPECT_LE(callback_finishes[0], records[0].completion);
    EXPECT_LE(callback_finishes[1], records[1].completion);
}

}  // namespace
