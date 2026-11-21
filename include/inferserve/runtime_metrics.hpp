#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

namespace inferserve {

struct RuntimeMetricsSnapshot {
    std::uint64_t queue_wait_samples = 0;
    std::chrono::nanoseconds queue_wait_total{};
    std::map<std::size_t, std::uint64_t> dispatched_batch_counts;
    std::uint64_t execution_samples = 0;
    std::chrono::nanoseconds execution_total{};
    std::uint64_t rejected_requests = 0;
};

// Bounded aggregate metrics for the server processing path.
class RuntimeMetrics {
public:
    void record_queue_wait(std::chrono::nanoseconds duration)
    {
        const std::lock_guard lock(mutex_);
        ++snapshot_.queue_wait_samples;
        snapshot_.queue_wait_total += duration;
    }

    void record_dispatched_batch(std::size_t batch_size)
    {
        const std::lock_guard lock(mutex_);
        ++snapshot_.dispatched_batch_counts[batch_size];
    }

    void record_execution(std::chrono::nanoseconds duration)
    {
        const std::lock_guard lock(mutex_);
        ++snapshot_.execution_samples;
        snapshot_.execution_total += duration;
    }

    void record_rejection()
    {
        const std::lock_guard lock(mutex_);
        ++snapshot_.rejected_requests;
    }

    [[nodiscard]] RuntimeMetricsSnapshot snapshot() const
    {
        const std::lock_guard lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    RuntimeMetricsSnapshot snapshot_;
};

}  // namespace inferserve
