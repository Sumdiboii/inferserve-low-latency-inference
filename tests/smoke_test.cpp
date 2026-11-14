#include <gtest/gtest.h>

#include <inferserve/bounded_queue.hpp>
#include <inferserve/core_types.hpp>
#include <inferserve/request_admission.hpp>

#include <chrono>
#include <optional>
#include <vector>

namespace {

using inferserve::Data;
using inferserve::BoundedQueue;
using inferserve::MonotonicClock;
using inferserve::Request;
using inferserve::RequestAdmission;
using inferserve::RequestId;
using inferserve::Response;
using inferserve::Status;
using inferserve::TimePoint;
using inferserve::kMaxInputSize;
using inferserve::validate_request;

TEST(CoreTypes, RequestIdDistinguishesIdentifiers)
{
    const RequestId first(1);
    const RequestId second(2);

    EXPECT_EQ(first, first);
    EXPECT_NE(first, second);
}

TEST(CoreTypes, RequestPreservesIdAndInput)
{
    const RequestId request_id(42);
    const Data input{1.0F, 2.0F, 3.0F};
    const Request request{request_id, input};

    EXPECT_EQ(request.id, request_id);
    EXPECT_EQ(request.input, input);
}

TEST(CoreTypes, ResponseRepresentsSuccessAndFailure)
{
    const RequestId request_id(7);
    const Data output{4.0F, 5.0F};
    const Response success(request_id, Status::Success, output);
    const Response failure(request_id, Status::BackendError, output);

    EXPECT_EQ(success.id, request_id);
    EXPECT_EQ(success.status, Status::Success);
    EXPECT_EQ(success.output, output);
    EXPECT_EQ(failure.status, Status::BackendError);
    EXPECT_TRUE(failure.output.empty());
}

TEST(CoreTypes, MonotonicClockReturnsComparableTimePoints)
{
    const MonotonicClock clock;
    const TimePoint first = clock.now();
    const TimePoint second = clock.now();

    EXPECT_LE(first, second);
}

TEST(CoreTypes, RequestInitializesOnlyArrivalTimestamp)
{
    const MonotonicClock clock;
    const TimePoint before = clock.now();
    const Request request(RequestId(9), Data{1.0F});
    const TimePoint after = clock.now();

    EXPECT_LE(before, request.arrival);
    EXPECT_LE(request.arrival, after);
    EXPECT_FALSE(request.enqueued.has_value());
    EXPECT_FALSE(request.batch_formed.has_value());
    EXPECT_FALSE(request.exec_start.has_value());
    EXPECT_FALSE(request.exec_end.has_value());
    EXPECT_FALSE(request.completed.has_value());
}

TEST(CoreTypes, RequestTimestampsCanBePopulatedChronologically)
{
    Request request(RequestId(10), Data{});
    const MonotonicClock clock;
    const TimePoint enqueued = clock.now();
    const TimePoint batch_formed = clock.now();
    const TimePoint exec_start = clock.now();
    const TimePoint exec_end = clock.now();
    const TimePoint completed = clock.now();

    request.enqueued = enqueued;
    request.batch_formed = batch_formed;
    request.exec_start = exec_start;
    request.exec_end = exec_end;
    request.completed = completed;

    ASSERT_TRUE(request.enqueued.has_value());
    ASSERT_TRUE(request.batch_formed.has_value());
    ASSERT_TRUE(request.exec_start.has_value());
    ASSERT_TRUE(request.exec_end.has_value());
    ASSERT_TRUE(request.completed.has_value());
    EXPECT_LE(request.arrival, *request.enqueued);
    EXPECT_LE(*request.enqueued, *request.batch_formed);
    EXPECT_LE(*request.batch_formed, *request.exec_start);
    EXPECT_LE(*request.exec_start, *request.exec_end);
    EXPECT_LE(*request.exec_end, *request.completed);
}

TEST(CoreTypes, ValidationAcceptsNonEmptyInputWithinLimit)
{
    const Request request(RequestId(11), Data(kMaxInputSize, 1.0F));

    EXPECT_EQ(validate_request(request), Status::Success);
}

TEST(CoreTypes, ValidationRejectsEmptyInput)
{
    const Request request(RequestId(12), Data{});

    EXPECT_EQ(validate_request(request), Status::InvalidInput);
}

TEST(CoreTypes, ValidationRejectsInputExceedingLimit)
{
    const Request request(RequestId(13), Data(kMaxInputSize + 1, 1.0F));

    EXPECT_EQ(validate_request(request), Status::InvalidInput);
}

TEST(CoreTypes, RequestCompletionReturnsFulfilledResponse)
{
    Request request(RequestId(14), Data{1.0F});
    std::future<Response> future = request.get_future();
    const Response expected(request.id, Status::Success, Data{2.0F});

    request.complete(expected);
    const Response actual = future.get();

    EXPECT_EQ(actual.id, request.id);
    EXPECT_EQ(actual.status, Status::Success);
    EXPECT_EQ(actual.output, expected.output);
}

TEST(CoreTypes, RequestCompletionSupportsFailedResponse)
{
    Request request(RequestId(15), Data{1.0F});
    std::future<Response> future = request.get_future();

    request.complete(Response(request.id, Status::BackendError));
    const Response actual = future.get();

    EXPECT_EQ(actual.id, request.id);
    EXPECT_EQ(actual.status, Status::BackendError);
    EXPECT_TRUE(actual.output.empty());
}

}  // namespace

TEST(GoogleTestIntegration, DiscoversAndRuns)
{
    EXPECT_EQ(1 + 1, 2);
}

TEST(BoundedQueue, PreservesFifoOrder)
{
    BoundedQueue<int> queue(3);

    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_TRUE(queue.try_push(3));

    int value = 0;
    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 1);
    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 3);
}

TEST(BoundedQueue, RejectsInsertionWhenFull)
{
    BoundedQueue<int> queue(2);

    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_EQ(queue.capacity(), 2U);
}

TEST(BoundedQueue, ReportsEmptyQueue)
{
    BoundedQueue<int> queue(1);
    int value = 0;

    EXPECT_FALSE(queue.try_pop(value));
}

TEST(BoundedQueue, BlockingPopRetrievesItemWhenAvailable)
{
    BoundedQueue<int> queue(1);
    std::future<std::optional<int>> popped =
        std::async(std::launch::async, [&queue] { return queue.pop(); });

    EXPECT_TRUE(queue.try_push(42));
    const std::optional<int> result = popped.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(BoundedQueue, TimedPopRetrievesItemWhenAvailable)
{
    BoundedQueue<int> queue(1);
    std::future<std::optional<int>> popped =
        std::async(std::launch::async, [&queue] { return queue.pop_for(std::chrono::seconds(1)); });

    EXPECT_TRUE(queue.try_push(42));
    const std::optional<int> result = popped.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(BoundedQueue, TimedPopReturnsEmptyAfterTimeout)
{
    BoundedQueue<int> queue(1);

    EXPECT_FALSE(queue.pop_for(std::chrono::milliseconds(20)).has_value());
}

TEST(BoundedQueue, TimedPopPreservesFifoOrder)
{
    BoundedQueue<int> queue(2);
    ASSERT_TRUE(queue.try_push(1));
    ASSERT_TRUE(queue.try_push(2));

    const std::optional<int> first = queue.pop_for(std::chrono::milliseconds(20));
    const std::optional<int> second = queue.pop_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, 1);
    EXPECT_EQ(*second, 2);
}

TEST(BoundedQueue, SupportsMoveOnlyRequests)
{
    BoundedQueue<Request> queue(1);
    Request request(RequestId(16), Data{1.0F});
    std::future<Response> future = request.get_future();

    ASSERT_TRUE(queue.try_push(std::move(request)));

    Request popped(RequestId(17), Data{});
    ASSERT_TRUE(queue.try_pop(popped));
    EXPECT_EQ(popped.id, RequestId(16));
    EXPECT_EQ(popped.input, Data({1.0F}));

    popped.complete(Response(popped.id, Status::Success));
    EXPECT_EQ(future.get().status, Status::Success);
}

TEST(BoundedQueue, RejectsPushesAfterShutdown)
{
    BoundedQueue<int> queue(1);
    queue.shutdown();
    queue.shutdown();

    EXPECT_FALSE(queue.try_push(42));
}

TEST(BoundedQueue, DrainsItemsAfterShutdown)
{
    BoundedQueue<int> queue(2);
    ASSERT_TRUE(queue.try_push(1));
    ASSERT_TRUE(queue.try_push(2));
    queue.shutdown();

    const std::optional<int> first = queue.pop();
    const std::optional<int> second = queue.pop();
    const std::optional<int> empty = queue.pop();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, 1);
    EXPECT_EQ(*second, 2);
    EXPECT_FALSE(empty.has_value());
}

TEST(BoundedQueue, BlockingPopWakesOnShutdown)
{
    BoundedQueue<int> queue(1);
    std::promise<void> started;
    std::future<void> started_future = started.get_future();
    std::future<std::optional<int>> popped = std::async(
        std::launch::async, [&queue, &started] {
            started.set_value();
            return queue.pop();
        });

    started_future.get();
    queue.shutdown();

    EXPECT_FALSE(popped.get().has_value());
}

TEST(BoundedQueue, TimedPopWakesOnShutdown)
{
    BoundedQueue<int> queue(1);
    std::promise<void> started;
    std::future<void> started_future = started.get_future();
    std::future<std::optional<int>> popped = std::async(
        std::launch::async, [&queue, &started] {
            started.set_value();
            return queue.pop_for(std::chrono::seconds(1));
        });

    started_future.get();
    queue.shutdown();

    EXPECT_FALSE(popped.get().has_value());
}

TEST(RequestAdmission, AcceptsRequestWhenCapacityIsAvailable)
{
    RequestAdmission admission(1);
    Request request(RequestId(18), Data{1.0F});
    std::future<Response> future = request.get_future();

    EXPECT_EQ(admission.admit(std::move(request)), Status::Success);

    std::optional<Request> queued = admission.pop();
    ASSERT_TRUE(queued.has_value());
    queued->complete(Response(queued->id, Status::Success));
    EXPECT_EQ(future.get().status, Status::Success);
}

TEST(RequestAdmission, RejectsFullQueueWithoutRemovingExistingRequest)
{
    RequestAdmission admission(1);
    Request first(RequestId(19), Data{1.0F});
    Request rejected(RequestId(20), Data{2.0F});
    std::future<Response> first_future = first.get_future();
    std::future<Response> rejected_future = rejected.get_future();

    ASSERT_EQ(admission.admit(std::move(first)), Status::Success);
    EXPECT_EQ(admission.admit(std::move(rejected)), Status::QueueFull);
    EXPECT_EQ(rejected_future.get().status, Status::QueueFull);

    std::optional<Request> queued = admission.pop();
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(queued->id, RequestId(19));
    queued->complete(Response(queued->id, Status::Success));
    EXPECT_EQ(first_future.get().status, Status::Success);
}

TEST(RequestAdmission, AcceptsRequestAfterCapacityBecomesAvailable)
{
    RequestAdmission admission(1);
    Request first(RequestId(21), Data{1.0F});
    Request second(RequestId(22), Data{2.0F});
    std::future<Response> first_future = first.get_future();
    std::future<Response> second_future = second.get_future();

    ASSERT_EQ(admission.admit(std::move(first)), Status::Success);
    std::optional<Request> queued = admission.pop();
    ASSERT_TRUE(queued.has_value());
    queued->complete(Response(queued->id, Status::Success));
    EXPECT_EQ(first_future.get().status, Status::Success);

    EXPECT_EQ(admission.admit(std::move(second)), Status::Success);
    queued = admission.pop();
    ASSERT_TRUE(queued.has_value());
    queued->complete(Response(queued->id, Status::Success));
    EXPECT_EQ(second_future.get().status, Status::Success);
}
