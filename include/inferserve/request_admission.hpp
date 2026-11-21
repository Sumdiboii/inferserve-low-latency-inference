#pragma once

#include <cstddef>
#include <chrono>
#include <optional>
#include <utility>

#include <inferserve/bounded_queue.hpp>
#include <inferserve/core_types.hpp>
#include <inferserve/runtime_metrics.hpp>

namespace inferserve {

class RequestAdmission {
public:
    explicit RequestAdmission(std::size_t capacity, RuntimeMetrics* metrics = nullptr)
        : queue_(capacity), metrics_(metrics)
    {
    }

    [[nodiscard]] Status admit(Request request)
    {
        const Status validation_status = validate_request(request);
        if (validation_status != Status::Success) {
            record_rejection();
            request.complete(Response(request.id, validation_status));
            return validation_status;
        }

        request.enqueued = MonotonicClock{}.now();
        const auto push_result = queue_.try_push_result(std::move(request));
        if (push_result != BoundedQueue<Request>::PushResult::Pushed) {
            request.enqueued.reset();
            const Status rejection_status = push_result == BoundedQueue<Request>::PushResult::Full
                ? Status::QueueFull
                : Status::ShuttingDown;
            record_rejection();
            request.complete(Response(request.id, rejection_status));
            return rejection_status;
        }

        return Status::Success;
    }

    [[nodiscard]] bool try_pop(Request& request) { return queue_.try_pop(request); }
    [[nodiscard]] std::optional<Request> pop() { return queue_.pop(); }

    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<Request> pop_for(std::chrono::duration<Rep, Period> timeout)
    {
        return queue_.pop_for(timeout);
    }

    void shutdown() { queue_.shutdown(); }

private:
    void record_rejection()
    {
        if (metrics_ != nullptr) {
            metrics_->record_rejection();
        }
    }

    BoundedQueue<Request> queue_;
    RuntimeMetrics* metrics_;
};

}  // namespace inferserve
