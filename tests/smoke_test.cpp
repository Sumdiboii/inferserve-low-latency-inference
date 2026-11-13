#include <gtest/gtest.h>

#include <inferserve/core_types.hpp>

#include <vector>

namespace {

using inferserve::Data;
using inferserve::Request;
using inferserve::RequestId;
using inferserve::Response;
using inferserve::Status;

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

}  // namespace

TEST(GoogleTestIntegration, DiscoversAndRuns)
{
    EXPECT_EQ(1 + 1, 2);
}
