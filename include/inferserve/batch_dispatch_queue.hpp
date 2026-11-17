#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <inferserve/bounded_queue.hpp>
#include <inferserve/core_types.hpp>

namespace inferserve {

class BatchDispatchQueue {
public:
    using RequestBatch = std::vector<Request>;
    using PushResult = BoundedQueue<RequestBatch>::PushResult;

    explicit BatchDispatchQueue(std::size_t capacity) : queue_(capacity) {}

    BatchDispatchQueue(const BatchDispatchQueue&) = delete;
    BatchDispatchQueue& operator=(const BatchDispatchQueue&) = delete;

    [[nodiscard]] bool try_push(RequestBatch&& batch)
    {
        return queue_.try_push(std::move(batch));
    }

    [[nodiscard]] PushResult try_push_result(RequestBatch&& batch)
    {
        return queue_.try_push_result(std::move(batch));
    }

    [[nodiscard]] bool try_pop(RequestBatch& batch) { return queue_.try_pop(batch); }
    [[nodiscard]] std::optional<RequestBatch> pop() { return queue_.pop(); }

    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<RequestBatch> pop_for(std::chrono::duration<Rep, Period> timeout)
    {
        return queue_.pop_for(timeout);
    }

    void shutdown() { queue_.shutdown(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return queue_.capacity(); }

private:
    BoundedQueue<RequestBatch> queue_;
};

}  // namespace inferserve
