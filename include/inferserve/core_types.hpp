#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <utility>
#include <vector>

namespace inferserve {

using TimePoint = std::chrono::steady_clock::time_point;

class MonotonicClock {
public:
    [[nodiscard]] TimePoint now() const { return std::chrono::steady_clock::now(); }
};

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

inline constexpr std::size_t kMaxInputSize = 4096;

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

struct Request {
    RequestId id;
    Data input;
    TimePoint arrival;
    std::optional<TimePoint> enqueued;
    std::optional<TimePoint> batch_formed;
    std::optional<TimePoint> exec_start;
    std::optional<TimePoint> exec_end;
    std::optional<TimePoint> completed;

    Request(RequestId request_id, Data request_input)
        : id(request_id), input(std::move(request_input)), arrival(MonotonicClock{}.now()) {}

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    Request(Request&&) = default;
    Request& operator=(Request&&) = default;

    // The submitter obtains the future; the processing side fulfils it with the response.
    [[nodiscard]] std::future<Response> get_future() { return completion_.get_future(); }
    void complete(Response response) { completion_.set_value(std::move(response)); }

private:
    std::promise<Response> completion_;
};

// Admission validation only checks the basic size boundary; shape and value validation belong later.
[[nodiscard]] inline Status validate_request(const Request& request)
{
    if (request.input.empty() || request.input.size() > kMaxInputSize) {
        return Status::InvalidInput;
    }

    return Status::Success;
}

}  // namespace inferserve
