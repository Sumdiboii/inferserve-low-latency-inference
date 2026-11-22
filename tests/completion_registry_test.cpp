#include <inferserve/completion_registry.hpp>
#include <inferserve/request_admission.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using inferserve::CompletionRegistry;
using inferserve::CompletionState;
using inferserve::RequestId;
using inferserve::Status;
using inferserve::RequestAdmission;
using inferserve::Request;
using inferserve::Data;

TEST(CompletionRegistry, RecordsPendingAndTerminalState)
{
    CompletionRegistry registry(4, std::chrono::minutes(1));
    RequestId id(42);

    registry.record_pending(id);
    const auto pending = registry.get(id);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, CompletionState::Pending);
    EXPECT_FALSE(pending->completed_at.has_value());
    EXPECT_FALSE(pending->status.has_value());

    registry.record_terminal(id, Status::Success);
    const auto terminal = registry.get(id);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, CompletionState::Terminal);
    ASSERT_TRUE(terminal->status.has_value());
    EXPECT_EQ(*terminal->status, Status::Success);
    EXPECT_TRUE(terminal->completed_at.has_value());
}

TEST(CompletionRegistry, KeepsPendingRequestsWhenTerminalCapacityIsFull)
{
    CompletionRegistry registry(1, std::chrono::minutes(1));

    registry.record_terminal(RequestId(1), Status::BackendError, "backend error");
    registry.record_pending(RequestId(2));
    registry.record_terminal(RequestId(3), Status::QueueFull, "queue full");

    EXPECT_FALSE(registry.get(RequestId(1)).has_value());
    EXPECT_TRUE(registry.get(RequestId(2)).has_value());
    const auto terminal = registry.get(RequestId(3));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, CompletionState::Terminal);
}

TEST(CompletionRegistry, PendingTransitionRemovesStaleTerminalOrderEntry)
{
    CompletionRegistry registry(2, std::chrono::minutes(1));
    RequestId id(13);

    registry.record_terminal(id, Status::Success);
    registry.record_pending(id);
    EXPECT_EQ(registry.get(id)->state, CompletionState::Pending);
    registry.record_terminal(id, Status::BackendError, "backend error");

    const auto terminal = registry.get(id);
    ASSERT_TRUE(terminal.has_value());
    ASSERT_TRUE(terminal->status.has_value());
    EXPECT_EQ(*terminal->status, Status::BackendError);
}

TEST(CompletionRegistry, RefreshesTerminalExpiryWhenRecordIsOverwritten)
{
    CompletionRegistry registry(4, std::chrono::milliseconds(100));
    RequestId id(12);

    registry.record_terminal(id, Status::Success);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    registry.record_terminal(id, Status::BackendError, "refreshed");
    std::this_thread::sleep_for(std::chrono::milliseconds(55));

    const auto record = registry.get(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->status.has_value());
    EXPECT_EQ(*record->status, Status::BackendError);
}

TEST(CompletionRegistry, ExpiresTerminalResultsButNotPendingResults)
{
    CompletionRegistry registry(4, std::chrono::milliseconds(5));

    registry.record_pending(RequestId(10));
    registry.record_terminal(RequestId(11), Status::BackendError, "backend error");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const auto pending = registry.get(RequestId(10));
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, CompletionState::Pending);
    EXPECT_FALSE(registry.get(RequestId(11)).has_value());
}

TEST(CompletionRegistry, RecordsTerminalFailureStatusAndMinimalError)
{
    CompletionRegistry registry(4, std::chrono::minutes(1));
    RequestId id(99);

    registry.record_pending(id);
    registry.record_terminal(id, Status::BackendError, "backend error");

    const auto record = registry.get(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->status.has_value());
    EXPECT_EQ(*record->status, Status::BackendError);
    EXPECT_EQ(record->error, "backend error");
}

TEST(CompletionRegistry, RequestAdmissionRecordsPendingAndQueueFullTerminalState)
{
    CompletionRegistry registry(4, std::chrono::minutes(1));
    RequestAdmission admission(1, nullptr, &registry);

    inferserve::Request request(RequestId(20), inferserve::Data{1.0F});
    EXPECT_EQ(admission.admit(std::move(request)), Status::Success);
    const auto pending = registry.get(RequestId(20));
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->state, CompletionState::Pending);

    inferserve::Request overflow(RequestId(21), inferserve::Data{1.0F});
    EXPECT_EQ(admission.admit(std::move(overflow)), Status::QueueFull);
    const auto terminal = registry.get(RequestId(21));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, CompletionState::Terminal);
    ASSERT_TRUE(terminal->status.has_value());
    EXPECT_EQ(*terminal->status, Status::QueueFull);
}

TEST(CompletionRegistry, RequestAdmissionRecordsShutdownTerminalState)
{
    CompletionRegistry registry(4, std::chrono::minutes(1));
    RequestAdmission admission(1, nullptr, &registry);
    admission.shutdown();

    Request request(RequestId(30), Data{1.0F});
    EXPECT_EQ(admission.admit(std::move(request)), Status::ShuttingDown);
    const auto terminal = registry.get(RequestId(30));
    ASSERT_TRUE(terminal.has_value());
    ASSERT_TRUE(terminal->status.has_value());
    EXPECT_EQ(*terminal->status, Status::ShuttingDown);
}

}  // namespace
