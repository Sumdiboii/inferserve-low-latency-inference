#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include <inferserve/bounded_queue.hpp>
#include <inferserve/core_types.hpp>

namespace inferserve {

class RequestAdmission {
public:
    explicit RequestAdmission(std::size_t capacity) : queue_(capacity) {}

    [[nodiscard]] Status admit(Request request)
    {
        const Status validation_status = validate_request(request);
        if (validation_status != Status::Success) {
            request.complete(Response(request.id, validation_status));
            return validation_status;
        }

        if (!queue_.try_push(std::move(request))) {
            request.complete(Response(request.id, Status::QueueFull));
            return Status::QueueFull;
        }

        return Status::Success;
    }

    [[nodiscard]] bool try_pop(Request& request) { return queue_.try_pop(request); }
    [[nodiscard]] std::optional<Request> pop() { return queue_.pop(); }
    void shutdown() { queue_.shutdown(); }

private:
    BoundedQueue<Request> queue_;
};

}  // namespace inferserve
