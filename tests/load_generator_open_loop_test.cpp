#include <gtest/gtest.h>

#include "open_loop_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using inferserve::load_generator::execute_open_loop;

TEST(OpenLoopExecutor, LaterArrivalStartsWhileEarlierExecutionIsStillBlocked)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool first_started = false;
    bool release_first = false;
    bool second_started_before_release = false;

    std::thread scheduler([&] {
        execute_open_loop({0.0, 0.01}, [&](std::size_t index, double) {
            std::unique_lock lock(mutex);
            if (index == 0) {
                first_started = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_first; });
            } else {
                second_started_before_release = !release_first;
                condition.notify_all();
            }
        });
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

}  // namespace
