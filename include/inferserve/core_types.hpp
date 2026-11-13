#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace inferserve {

// A request identifier is deliberately not implicitly interchangeable with an integer.
class RequestId {
public:
    explicit constexpr RequestId(std::uint64_t value) : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const { return value_; }

    friend constexpr bool operator==(RequestId, RequestId) = default;

private:
    std::uint64_t value_;
};

enum class Status {
    Success,
    InvalidInput,
    QueueFull,
    BackendError,
    InternalError,
};

using Data = std::vector<float>;

struct Request {
    RequestId id;
    Data input;
};

struct Response {
    RequestId id;
    Status status;
    // Output is meaningful only for Success; failures always carry an empty output.
    Data output;

    Response(RequestId request_id, Status response_status, Data response_output = {})
        : id(request_id),
          status(response_status),
          output(response_status == Status::Success ? std::move(response_output) : Data{}) {}
};

}  // namespace inferserve
