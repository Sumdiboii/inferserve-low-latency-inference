#include <gtest/gtest.h>

#include <inferserve/core_types.hpp>

#include <vector>

namespace {

using inferserve::Data;
using inferserve::MonotonicClock;
using inferserve::Request;
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
