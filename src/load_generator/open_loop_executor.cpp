#include "open_loop_executor.hpp"

#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace inferserve::load_generator {

void execute_open_loop(const std::vector<double>& scheduled_arrivals_seconds,
                       const std::function<void(std::size_t, double)>& execute_request)
{
    using Clock = std::chrono::steady_clock;
    const auto schedule_start = Clock::now();
    std::vector<std::future<void>> executions;
    executions.reserve(scheduled_arrivals_seconds.size());

    for (std::size_t index = 0; index < scheduled_arrivals_seconds.size(); ++index) {
        const double scheduled_seconds = scheduled_arrivals_seconds[index];
        const auto offset = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(scheduled_seconds));
        std::this_thread::sleep_until(schedule_start + offset);
        executions.push_back(std::async(std::launch::async, [&execute_request, index, scheduled_seconds] {
            execute_request(index, scheduled_seconds);
        }));
    }

    for (auto& execution : executions) {
        execution.get();
    }
}

}  // namespace inferserve::load_generator
