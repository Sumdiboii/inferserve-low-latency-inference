#include <gtest/gtest.h>

#include <inferserve/backend.hpp>
#include <inferserve/batcher.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/batch_worker_pool.hpp>
#include <inferserve/bounded_queue.hpp>
#include <inferserve/core_types.hpp>
#include <inferserve/request_admission.hpp>
#include <inferserve/onnx_runtime_backend.hpp>
#include <inferserve/synthetic_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using inferserve::Data;
using inferserve::Backend;
using inferserve::Batch;
using inferserve::Batcher;
using inferserve::BatchDispatchQueue;
using inferserve::BatchWorkerPool;
using inferserve::BoundedQueue;
using inferserve::InferenceResults;
using inferserve::MonotonicClock;
using inferserve::OnnxRuntimeBackend;
using inferserve::Request;
using inferserve::RequestAdmission;
using inferserve::RequestId;
using inferserve::Response;
using inferserve::Status;
using inferserve::SyntheticBackend;
using inferserve::SyntheticBackendConfig;
using inferserve::TimePoint;
using inferserve::kMaxInputSize;
using inferserve::validate_request;

class TestBackend final : public Backend {
public:
    void load() override { loaded_ = true; }

    [[nodiscard]] InferenceResults infer(const Batch& batch) override
    {
        EXPECT_TRUE(loaded_);
        return batch;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return 4; }

    [[nodiscard]] std::string_view name() const noexcept override { return "test"; }

private:
    bool loaded_ = false;
};

class ThrowingBackend final : public Backend {
public:
    void load() override {}
    [[nodiscard]] InferenceResults infer(const Batch&) override
    {
        throw std::runtime_error("inference failed");
    }
    [[nodiscard]] std::size_t max_batch_size() const override { return 1; }
    [[nodiscard]] std::string_view name() const noexcept override { return "throwing"; }
};

class ConcurrentBackend final : public Backend {
public:
    void load() override {}

    [[nodiscard]] InferenceResults infer(const Batch& batch) override
    {
        std::unique_lock lock(mutex_);
        processed_inputs_.insert(processed_inputs_.end(), batch.begin(), batch.end());
        ++active_;
        if (active_ > maximum_active_) {
            maximum_active_ = active_;
        }
        both_active_.notify_all();
        both_active_.wait(lock, [this] { return maximum_active_ == 2; });
        --active_;
        return batch;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return 4; }
    [[nodiscard]] std::string_view name() const noexcept override { return "concurrent"; }
    [[nodiscard]] std::size_t maximum_active() const
    {
        std::lock_guard lock(mutex_);
        return maximum_active_;
    }
    [[nodiscard]] Batch processed_inputs() const
    {
        std::lock_guard lock(mutex_);
        return processed_inputs_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable both_active_;
    std::size_t active_ = 0;
    std::size_t maximum_active_ = 0;
    Batch processed_inputs_;
};

class GatedBackend final : public Backend {
public:
    GatedBackend() : release_(release_promise_.get_future().share()) {}

    void load() override {}

    [[nodiscard]] InferenceResults infer(const Batch& batch) override
    {
        entered_.set_value();
        release_.wait();
        return batch;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return 4; }
    [[nodiscard]] std::string_view name() const noexcept override { return "gated"; }
    [[nodiscard]] std::future<void> entered() { return entered_.get_future(); }
    void release() { release_promise_.set_value(); }

private:
    std::promise<void> entered_;
    std::promise<void> release_promise_;
    std::shared_future<void> release_;
};

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

TEST(Backend, ExposesLoadInferenceAndMetadataContract)
{
    TestBackend backend;
    const Batch batch{Data{1.0F, 2.0F}, Data{3.0F}};

    EXPECT_EQ(backend.name(), "test");
    EXPECT_EQ(backend.max_batch_size(), 4U);

    backend.load();
    const InferenceResults results = backend.infer(batch);
    EXPECT_EQ(results, batch);
}

TEST(OnnxRuntimeBackend, LoadsFixedShapeModelAndRunsOneRequest)
{
    OnnxRuntimeBackend backend(INFERSERVE_TEST_MODEL_PATH);
    backend.load();

    EXPECT_EQ(backend.name(), "onnxruntime");
    EXPECT_EQ(backend.max_batch_size(), 1U);
    EXPECT_EQ(backend.input_shape(), (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(backend.output_shape(), (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(backend.infer(Batch{Data{3.5F, -2.0F}}), (InferenceResults{Data{3.5F, -2.0F}}));
}

TEST(OnnxRuntimeBackend, ReportsModelLoadingFailureWithPath)
{
    OnnxRuntimeBackend backend("missing-model.onnx");

    try {
        backend.load();
        FAIL() << "expected model loading to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("missing-model.onnx"), std::string::npos);
    }
}

TEST(OnnxRuntimeBackend, PacksAndScattersDynamicBatchInRequestOrder)
{
    OnnxRuntimeBackend backend(INFERSERVE_TEST_DYNAMIC_BATCH_MODEL_PATH);
    backend.load();
    ASSERT_GT(backend.max_batch_size(), 1U);

    const Batch batch{Data{1.25F, -4.0F}, Data{9.5F, 2.0F}, Data{-7.0F, 3.75F}};
    const InferenceResults results = backend.infer(batch);

    ASSERT_EQ(results.size(), batch.size());
    for (std::size_t index = 0; index < batch.size(); ++index) {
        EXPECT_EQ(results[index], batch[index]);
    }
}

TEST(OnnxRuntimeBackend, MatchesPythonIdentityReferenceOutputs)
{
    struct ReferenceCase {
        std::string name;
        Batch inputs;
        InferenceResults expected;
    };

    std::ifstream reference_file(INFERSERVE_TEST_PYTHON_REFERENCE_PATH);
    ASSERT_TRUE(reference_file) << "could not open Python reference data";

    std::vector<ReferenceCase> cases;
    std::string name;
    float input0 = 0.0F;
    float input1 = 0.0F;
    float expected0 = 0.0F;
    float expected1 = 0.0F;
    while (reference_file >> name >> input0 >> input1 >> expected0 >> expected1) {
        if (cases.empty() || cases.back().name != name) {
            cases.push_back(ReferenceCase{name, {}, {}});
        }
        cases.back().inputs.push_back(Data{input0, input1});
        cases.back().expected.push_back(Data{expected0, expected1});
    }
    ASSERT_TRUE(reference_file.eof()) << "malformed Python reference data";
    ASSERT_EQ(cases.size(), 2U);
    EXPECT_EQ(cases[0].name, "batch_one");
    EXPECT_EQ(cases[0].inputs.size(), 1U);
    EXPECT_EQ(cases[1].name, "batch_three");
    EXPECT_EQ(cases[1].inputs.size(), 3U);

    OnnxRuntimeBackend backend(INFERSERVE_TEST_DYNAMIC_BATCH_MODEL_PATH);
    backend.load();
    for (const ReferenceCase& reference : cases) {
        const InferenceResults actual = backend.infer(reference.inputs);
        ASSERT_EQ(actual.size(), reference.expected.size()) << reference.name;
        for (std::size_t request = 0; request < actual.size(); ++request) {
            ASSERT_EQ(actual[request].size(), reference.expected[request].size()) << reference.name;
            for (std::size_t element = 0; element < actual[request].size(); ++element) {
                const float expected = reference.expected[request][element];
                const float tolerance = 1.0e-6F + 1.0e-5F * std::abs(expected);
                EXPECT_NEAR(actual[request][element], expected, tolerance)
                    << reference.name << " request " << request << " element " << element;
            }
        }
    }
}

TEST(OnnxRuntimeBackend, RejectsInputsWithWrongFeatureElementCount)
{
    OnnxRuntimeBackend backend(INFERSERVE_TEST_DYNAMIC_BATCH_MODEL_PATH);
    backend.load();

    EXPECT_THROW(static_cast<void>(backend.infer(Batch{Data{1.0F, 2.0F, 3.0F}})), std::invalid_argument);
}

TEST(OnnxRuntimeBackend, FixedBatchOneModelRejectsLargerBatches)
{
    OnnxRuntimeBackend backend(INFERSERVE_TEST_MODEL_PATH);
    backend.load();

    EXPECT_EQ(backend.max_batch_size(), 1U);
    EXPECT_THROW(static_cast<void>(backend.infer(Batch{Data{1.0F, 2.0F}, Data{3.0F, 4.0F}})),
        std::invalid_argument);
}

TEST(SyntheticBackend, ProducesOrderedDeterministicResults)
{
    const SyntheticBackendConfig config{std::chrono::milliseconds(1), std::chrono::milliseconds(1), 4};
    SyntheticBackend backend(config);
    const Batch batch{Data{1.0F}, Data{2.0F, 3.0F}, Data{4.0F, 5.0F, 6.0F}};

    EXPECT_EQ(backend.name(), "synthetic");
    EXPECT_EQ(backend.max_batch_size(), 4U);

    backend.load();
    const auto started = std::chrono::steady_clock::now();
    const InferenceResults results = backend.infer(batch);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(results.size(), batch.size());
    ASSERT_EQ(results[0], Data({1.0F}));
    ASSERT_EQ(results[1], Data({2.0F}));
    ASSERT_EQ(results[2], Data({3.0F}));
    EXPECT_GE(elapsed, std::chrono::milliseconds(3));
}

TEST(SyntheticBackend, ProvidesDefaultConfiguration)
{
    const SyntheticBackendConfig config;
    SyntheticBackend backend;

    EXPECT_EQ(config.fixed_ms, std::chrono::milliseconds(1));
    EXPECT_EQ(config.per_item_ms, std::chrono::milliseconds(1));
    EXPECT_EQ(config.max_batch_size, 8U);
    EXPECT_EQ(backend.max_batch_size(), config.max_batch_size);
}

TEST(SyntheticBackend, CustomTimingParametersAffectExecutionCost)
{
    const SyntheticBackendConfig config{std::chrono::milliseconds(2), std::chrono::milliseconds(3), 4};
    SyntheticBackend backend(config);
    const Batch batch{Data{1.0F}, Data{2.0F}};

    backend.load();
    const auto started = std::chrono::steady_clock::now();
    static_cast<void>(backend.infer(batch));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // The configured cost is 2 ms + 3 ms * 2 items; allow 1 ms for clock jitter.
    EXPECT_GE(elapsed, std::chrono::milliseconds(7));
}

TEST(SyntheticBackend, RejectsInvalidConfiguration)
{
    EXPECT_THROW(
        SyntheticBackend(SyntheticBackendConfig{std::chrono::milliseconds(-1), std::chrono::milliseconds(1), 4}),
        std::invalid_argument);
    EXPECT_THROW(
        SyntheticBackend(SyntheticBackendConfig{std::chrono::milliseconds(1), std::chrono::milliseconds(-1), 4}),
        std::invalid_argument);
    EXPECT_THROW(
        SyntheticBackend(SyntheticBackendConfig{std::chrono::milliseconds(1), std::chrono::milliseconds(1), 0}),
        std::invalid_argument);
}

TEST(Batcher, FormsBatchesUpToConfiguredSize)
{
    RequestAdmission admission(4);
    std::condition_variable batches_ready;
    std::mutex batches_mutex;
    std::vector<std::vector<RequestId>> batches;

    Batcher batcher(admission, 2, [&](const Batcher::RequestBatch& batch) {
        std::vector<RequestId> ids;
        for (const Request& request : batch) {
            ids.push_back(request.id);
        }

        {
            std::lock_guard lock(batches_mutex);
            batches.push_back(std::move(ids));
        }
        batches_ready.notify_one();
    });

    for (std::uint64_t id = 1; id <= 3; ++id) {
        Request request(RequestId(id), Data{1.0F});
        ASSERT_EQ(admission.admit(std::move(request)), Status::Success);
    }

    batcher.start();
    {
        std::unique_lock lock(batches_mutex);
        ASSERT_TRUE(batches_ready.wait_for(lock, std::chrono::seconds(1), [&batches] {
            return batches.size() == 2;
        }));
    }
    batcher.stop();

    ASSERT_EQ(batches.size(), 2U);
    ASSERT_EQ(batches[0], std::vector<RequestId>({RequestId(1), RequestId(2)}));
    EXPECT_EQ(batches[1], std::vector<RequestId>({RequestId(3)}));
}

TEST(Batcher, DispatchesEachRequestIndividuallyWhenBatchingIsOff)
{
    RequestAdmission admission(3);
    std::condition_variable batches_ready;
    std::mutex batches_mutex;
    std::vector<std::vector<RequestId>> batches;

    Batcher batcher(admission, 1, [&](const Batcher::RequestBatch& batch) {
        std::vector<RequestId> ids;
        for (const Request& request : batch) {
            ids.push_back(request.id);
        }

        {
            std::lock_guard lock(batches_mutex);
            batches.push_back(std::move(ids));
        }
        batches_ready.notify_one();
    });

    for (std::uint64_t id = 1; id <= 3; ++id) {
        ASSERT_EQ(admission.admit(Request(RequestId(id), Data{1.0F})), Status::Success);
    }

    batcher.start();
    {
        std::unique_lock lock(batches_mutex);
        ASSERT_TRUE(batches_ready.wait_for(lock, std::chrono::seconds(1), [&batches] {
            return batches.size() == 3;
        }));
    }
    batcher.stop();

    ASSERT_EQ(batches.size(), 3U);
    EXPECT_EQ(batches[0], std::vector<RequestId>({RequestId(1)}));
    EXPECT_EQ(batches[1], std::vector<RequestId>({RequestId(2)}));
    EXPECT_EQ(batches[2], std::vector<RequestId>({RequestId(3)}));
}

TEST(Batcher, DispatchesFormedBatchToDispatchQueue)
{
    RequestAdmission admission(2);
    BatchDispatchQueue dispatch_queue(1);
    Batcher batcher(admission, dispatch_queue, 2);

    Request request(RequestId(4), Data{1.0F});
    std::future<Response> response = request.get_future();
    ASSERT_EQ(admission.admit(std::move(request)), Status::Success);

    batcher.start();
    std::optional<BatchDispatchQueue::RequestBatch> batch =
        dispatch_queue.pop_for(std::chrono::seconds(1));
    batcher.stop();

    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->size(), 1U);
    EXPECT_EQ(batch->front().id, RequestId(4));
    EXPECT_TRUE(batch->front().batch_formed.has_value());

    batch->front().complete(Response(batch->front().id, Status::Success));
    EXPECT_EQ(response.get().status, Status::Success);
}

TEST(Batcher, CompletesRequestsWhenDispatchQueueIsShutdown)
{
    RequestAdmission admission(1);
    BatchDispatchQueue dispatch_queue(1);
    dispatch_queue.shutdown();
    Batcher batcher(admission, dispatch_queue, 1);

    Request request(RequestId(22), Data{1.0F});
    std::future<Response> response = request.get_future();
    ASSERT_EQ(admission.admit(std::move(request)), Status::Success);

    batcher.start();
    batcher.stop();

    EXPECT_EQ(response.get().status, Status::InternalError);
}

TEST(Batcher, StopShutsDownWaitingBatcherThread)
{
    RequestAdmission admission(1);
    Batcher batcher(admission, 2, [](const Batcher::RequestBatch&) {});

    batcher.start();
    batcher.stop();

    Request request(RequestId(4), Data{1.0F});
    std::future<Response> future = request.get_future();
    EXPECT_EQ(admission.admit(std::move(request)), Status::QueueFull);
    EXPECT_EQ(future.get().status, Status::QueueFull);
}

TEST(Batcher, DispatchesPartialBatchAfterMaximumDelay)
{
    RequestAdmission admission(2);
    std::condition_variable batch_ready;
    std::mutex batch_mutex;
    std::vector<RequestId> batch_ids;
    const auto max_delay = std::chrono::milliseconds(30);

    Batcher batcher(admission, 2, [&](const Batcher::RequestBatch& batch) {
        std::lock_guard lock(batch_mutex);
        for (const Request& request : batch) {
            batch_ids.push_back(request.id);
        }
        batch_ready.notify_one();
    }, max_delay);

    Request request(RequestId(5), Data{1.0F});
    ASSERT_EQ(admission.admit(std::move(request)), Status::Success);

    const auto started = std::chrono::steady_clock::now();
    batcher.start();
    {
        std::unique_lock lock(batch_mutex);
        ASSERT_TRUE(batch_ready.wait_for(lock, std::chrono::seconds(1), [&batch_ids] {
            return !batch_ids.empty();
        }));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    batcher.stop();

    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
    EXPECT_EQ(batch_ids, std::vector<RequestId>({RequestId(5)}));
}

TEST(Batcher, ShutdownDispatchesPendingPartialBatch)
{
    RequestAdmission admission(2);
    TestBackend backend;
    backend.load();
    Batcher batcher(admission, backend, 2);

    Request request(RequestId(6), Data{1.0F});
    std::future<Response> response = request.get_future();
    ASSERT_EQ(admission.admit(std::move(request)), Status::Success);
    batcher.start();
    batcher.stop();

    const Response result = response.get();
    EXPECT_EQ(result.id, RequestId(6));
    EXPECT_EQ(result.status, Status::Success);
    EXPECT_EQ(result.output, Data{1.0F});
}

TEST(Batcher, SeparatesRequestsWithDifferentInputSizes)
{
    RequestAdmission admission(4);
    std::condition_variable batches_ready;
    std::mutex batches_mutex;
    std::vector<std::vector<RequestId>> batches;

    Batcher batcher(admission, 3, [&](const Batcher::RequestBatch& batch) {
        std::vector<RequestId> ids;
        for (const Request& request : batch) {
            ids.push_back(request.id);
        }

        {
            std::lock_guard lock(batches_mutex);
            batches.push_back(std::move(ids));
        }
        batches_ready.notify_one();
    });

    ASSERT_EQ(admission.admit(Request(RequestId(6), Data{1.0F, 2.0F})), Status::Success);
    ASSERT_EQ(admission.admit(Request(RequestId(7), Data{3.0F})), Status::Success);
    ASSERT_EQ(admission.admit(Request(RequestId(8), Data{4.0F})), Status::Success);

    batcher.start();
    {
        std::unique_lock lock(batches_mutex);
        ASSERT_TRUE(batches_ready.wait_for(lock, std::chrono::seconds(1), [&batches] {
            return batches.size() == 2;
        }));
    }
    batcher.stop();

    ASSERT_EQ(batches.size(), 2U);
    EXPECT_EQ(batches[0], std::vector<RequestId>({RequestId(6)}));
    EXPECT_EQ(batches[1], std::vector<RequestId>({RequestId(7), RequestId(8)}));
}

TEST(Batcher, DeliversBackendResultsToRequestFutures)
{
    RequestAdmission admission(2);
    TestBackend backend;
    backend.load();
    Batcher batcher(admission, backend, 2);

    Request first(RequestId(9), Data{1.0F, 2.0F});
    Request second(RequestId(10), Data{3.0F, 4.0F});
    std::future<Response> first_future = first.get_future();
    std::future<Response> second_future = second.get_future();

    ASSERT_EQ(admission.admit(std::move(first)), Status::Success);
    ASSERT_EQ(admission.admit(std::move(second)), Status::Success);

    batcher.start();
    const Response first_response = first_future.get();
    const Response second_response = second_future.get();
    batcher.stop();

    EXPECT_EQ(first_response.id, RequestId(9));
    EXPECT_EQ(first_response.status, Status::Success);
    EXPECT_EQ(first_response.output, Data({1.0F, 2.0F}));
    EXPECT_EQ(second_response.id, RequestId(10));
    EXPECT_EQ(second_response.status, Status::Success);
    EXPECT_EQ(second_response.output, Data({3.0F, 4.0F}));
}

TEST(Batcher, ObserverExceptionDoesNotRepeatCompletionOrStopLaterBatches)
{
    RequestAdmission admission(2);
    TestBackend backend;
    backend.load();
    std::size_t observer_calls = 0;
    Batcher batcher(admission, backend, 1, [&](const Batcher::RequestBatch&) {
        ++observer_calls;
        if (observer_calls == 1) {
            throw std::runtime_error("observer failed");
        }
    });

    Request first(RequestId(13), Data{1.0F});
    Request second(RequestId(14), Data{2.0F});
    std::future<Response> first_response = first.get_future();
    std::future<Response> second_response = second.get_future();
    ASSERT_EQ(admission.admit(std::move(first)), Status::Success);
    ASSERT_EQ(admission.admit(std::move(second)), Status::Success);

    batcher.start();
    const bool first_completed = first_response.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    const bool second_completed = second_response.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    batcher.stop();

    ASSERT_TRUE(first_completed);
    ASSERT_TRUE(second_completed);
    EXPECT_EQ(first_response.get().status, Status::Success);
    EXPECT_EQ(second_response.get().status, Status::Success);
    EXPECT_EQ(observer_calls, 2U);
}

TEST(Batcher, RecordsBackendExecutionTimestamps)
{
    RequestAdmission admission(1);
    TestBackend backend;
    backend.load();
    std::condition_variable batch_ready;
    std::mutex batch_mutex;
    std::optional<TimePoint> arrival;
    std::optional<TimePoint> enqueued;
    std::optional<TimePoint> batch_formed;
    std::optional<TimePoint> exec_start;
    std::optional<TimePoint> exec_end;
    std::optional<TimePoint> completed;

    Batcher batcher(admission, backend, 1, [&](const Batcher::RequestBatch& batch) {
        {
            std::lock_guard lock(batch_mutex);
            arrival = batch.front().arrival;
            enqueued = batch.front().enqueued;
            batch_formed = batch.front().batch_formed;
            exec_start = batch.front().exec_start;
            exec_end = batch.front().exec_end;
            completed = batch.front().completed;
        }
        batch_ready.notify_one();
    });

    Request request(RequestId(11), Data{1.0F});
    std::future<Response> response = request.get_future();
    ASSERT_EQ(admission.admit(std::move(request)), Status::Success);

    batcher.start();
    {
        std::unique_lock lock(batch_mutex);
        ASSERT_TRUE(batch_ready.wait_for(lock, std::chrono::seconds(1), [&completed] {
            return completed.has_value();
        }));
    }
    EXPECT_EQ(response.get().status, Status::Success);
    batcher.stop();

    ASSERT_TRUE(arrival.has_value());
    ASSERT_TRUE(enqueued.has_value());
    ASSERT_TRUE(batch_formed.has_value());
    ASSERT_TRUE(exec_start.has_value());
    ASSERT_TRUE(exec_end.has_value());
    ASSERT_TRUE(completed.has_value());
    EXPECT_LE(*arrival, *enqueued);
    EXPECT_LE(*enqueued, *batch_formed);
    EXPECT_LE(*batch_formed, *exec_start);
    EXPECT_LE(*exec_start, *exec_end);
    EXPECT_LE(*exec_end, *completed);
}

TEST(Batcher, RecordsCompletionTimestampsWhenBackendThrows)
{
    RequestAdmission admission(1);
    ThrowingBackend backend;
    std::condition_variable batch_ready;
    std::mutex batch_mutex;
    std::optional<TimePoint> exec_end;
    std::optional<TimePoint> completed;

    Batcher batcher(admission, backend, 1, [&](const Batcher::RequestBatch& batch) {
        {
            std::lock_guard lock(batch_mutex);
            exec_end = batch.front().exec_end;
            completed = batch.front().completed;
        }
        batch_ready.notify_one();
    });

    Request request(RequestId(12), Data{1.0F});
    std::future<Response> response = request.get_future();
    ASSERT_EQ(admission.admit(std::move(request)), Status::Success);

    batcher.start();
    {
        std::unique_lock lock(batch_mutex);
        ASSERT_TRUE(batch_ready.wait_for(lock, std::chrono::seconds(1), [&completed] {
            return completed.has_value();
        }));
    }
    EXPECT_EQ(response.get().status, Status::BackendError);
    batcher.stop();

    ASSERT_TRUE(exec_end.has_value());
    ASSERT_TRUE(completed.has_value());
    EXPECT_LE(*exec_end, *completed);
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

TEST(BatchDispatchQueue, MovesFormedBatchAndPreservesRequestOwnership)
{
    BatchDispatchQueue queue(1);
    BatchDispatchQueue::RequestBatch batch;
    batch.emplace_back(RequestId(18), Data{1.0F, 2.0F});
    std::future<Response> future = batch.front().get_future();

    ASSERT_TRUE(queue.try_push(std::move(batch)));
    std::optional<BatchDispatchQueue::RequestBatch> popped = queue.pop();

    ASSERT_TRUE(popped.has_value());
    ASSERT_EQ(popped->size(), 1U);
    EXPECT_EQ(popped->front().id, RequestId(18));
    EXPECT_EQ(popped->front().input, Data({1.0F, 2.0F}));

    popped->front().complete(Response(popped->front().id, Status::Success));
    EXPECT_EQ(future.get().status, Status::Success);
}

TEST(BatchDispatchQueue, DistinguishesFullAndShutdownPushes)
{
    BatchDispatchQueue queue(1);
    BatchDispatchQueue::RequestBatch first_batch;
    first_batch.emplace_back(RequestId(19), Data{1.0F});
    ASSERT_EQ(queue.try_push_result(std::move(first_batch)),
              BatchDispatchQueue::PushResult::Pushed);

    BatchDispatchQueue::RequestBatch full_batch;
    full_batch.emplace_back(RequestId(20), Data{2.0F});
    EXPECT_EQ(queue.try_push_result(std::move(full_batch)), BatchDispatchQueue::PushResult::Full);

    queue.shutdown();
    BatchDispatchQueue::RequestBatch shutdown_batch;
    shutdown_batch.emplace_back(RequestId(21), Data{3.0F});
    EXPECT_EQ(queue.try_push_result(std::move(shutdown_batch)),
              BatchDispatchQueue::PushResult::Shutdown);
}

TEST(BatchWorkerPool, ConfiguredWorkersProcessBatchesConcurrently)
{
    BatchDispatchQueue dispatch_queue(2);
    ConcurrentBackend backend;
    BatchWorkerPool workers(dispatch_queue, backend, 2);
    std::vector<std::future<Response>> completions;

    for (std::uint64_t id = 1; id <= 2; ++id) {
        BatchDispatchQueue::RequestBatch batch;
        Request request(RequestId(id), Data{static_cast<float>(id)});
        completions.push_back(request.get_future());
        batch.push_back(std::move(request));
        ASSERT_TRUE(dispatch_queue.try_push(std::move(batch)));
    }

    workers.start();
    workers.shutdown();
    workers.join();

    EXPECT_EQ(backend.maximum_active(), 2U);
    Batch processed_inputs = backend.processed_inputs();
    std::sort(processed_inputs.begin(), processed_inputs.end());
    EXPECT_EQ(processed_inputs, Batch({Data{1.0F}, Data{2.0F}}));
    for (std::size_t index = 0; index < completions.size(); ++index) {
        const Response response = completions[index].get();
        EXPECT_EQ(response.id, RequestId(index + 1));
        EXPECT_EQ(response.status, Status::Success);
        EXPECT_EQ(response.output, Data({static_cast<float>(index + 1)}));
    }
}

TEST(BatchWorkerPool, ShutdownWakesWorkersWaitingOnEmptyQueue)
{
    BatchDispatchQueue dispatch_queue(1);
    TestBackend backend;
    backend.load();
    BatchWorkerPool workers(dispatch_queue, backend, 2);

    workers.start();
    workers.shutdown();
    workers.join();

    EXPECT_THROW(workers.start(), std::logic_error);
}

TEST(BatchWorkerPool, ShutdownDrainsBatchesAlreadyQueued)
{
    BatchDispatchQueue dispatch_queue(2);
    TestBackend backend;
    backend.load();
    BatchWorkerPool workers(dispatch_queue, backend, 1);
    std::vector<std::future<Response>> responses;

    for (std::uint64_t id = 10; id < 12; ++id) {
        BatchDispatchQueue::RequestBatch batch;
        Request request(RequestId(id), Data{static_cast<float>(id)});
        responses.push_back(request.get_future());
        batch.push_back(std::move(request));
        ASSERT_TRUE(dispatch_queue.try_push(std::move(batch)));
    }

    workers.start();
    workers.shutdown();
    workers.shutdown();
    workers.join();
    workers.join();

    for (std::size_t index = 0; index < responses.size(); ++index) {
        const Response response = responses[index].get();
        EXPECT_EQ(response.status, Status::Success);
        EXPECT_EQ(response.id, RequestId(index + 10));
    }
}

TEST(BatchWorkerPool, InFlightInferenceFinishesDuringShutdown)
{
    BatchDispatchQueue dispatch_queue(1);
    GatedBackend backend;
    std::future<void> entered = backend.entered();
    BatchWorkerPool workers(dispatch_queue, backend, 1);
    BatchDispatchQueue::RequestBatch batch;
    Request request(RequestId(12), Data{3.0F});
    std::future<Response> response = request.get_future();
    batch.push_back(std::move(request));
    ASSERT_TRUE(dispatch_queue.try_push(std::move(batch)));

    workers.start();
    if (entered.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
        backend.release();
        workers.shutdown();
        workers.join();
        FAIL() << "worker did not enter inference";
    }
    workers.shutdown();
    EXPECT_EQ(response.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    backend.release();
    workers.join();

    EXPECT_EQ(response.get().status, Status::Success);
}

TEST(BatchWorkerPool, RejectsJoinBeforeShutdownAndShutdownBeforeStart)
{
    BatchDispatchQueue dispatch_queue(1);
    TestBackend backend;
    BatchWorkerPool workers(dispatch_queue, backend, 1);

    EXPECT_THROW(workers.shutdown(), std::logic_error);
    EXPECT_THROW(workers.join(), std::logic_error);
    workers.start();
    EXPECT_THROW(workers.join(), std::logic_error);
    workers.shutdown();
    workers.join();
}

TEST(BatchWorkerPool, DestructorHandlesUnstartedAndDrainsStartedPool)
{
    BatchDispatchQueue unstarted_queue(1);
    TestBackend backend;
    backend.load();
    { BatchWorkerPool unstarted(unstarted_queue, backend, 1); }

    BatchDispatchQueue dispatch_queue(1);
    BatchDispatchQueue::RequestBatch batch;
    Request request(RequestId(13), Data{4.0F});
    std::future<Response> response = request.get_future();
    batch.push_back(std::move(request));
    ASSERT_TRUE(dispatch_queue.try_push(std::move(batch)));

    {
        BatchWorkerPool started(dispatch_queue, backend, 1);
        started.start();
    }

    const Response result = response.get();
    EXPECT_EQ(result.status, Status::Success);
    EXPECT_EQ(result.id, RequestId(13));
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

TEST(BoundedQueue, SupportsRepeatedMpmcProductionAndConsumption)
{
    constexpr int producer_count = 4;
    constexpr int consumer_count = 4;
    constexpr int items_per_producer = 100;
    constexpr int default_repetitions = 20;
    constexpr std::size_t queue_capacity = 8;
    constexpr int items_per_round = producer_count * items_per_producer;
    int repetitions = default_repetitions;
    if (const char* configured_repetitions = std::getenv("INFERSERVE_MPMC_REPETITIONS");
        configured_repetitions != nullptr) {
        const int parsed_repetitions = std::atoi(configured_repetitions);
        if (parsed_repetitions > 0) {
            repetitions = parsed_repetitions;
        }
    }

    for (int repetition = 0; repetition < repetitions; ++repetition) {
        BoundedQueue<int> queue(queue_capacity);
        std::mutex consumed_mutex;
        std::vector<int> consumed;
        consumed.reserve(items_per_round);

        std::promise<void> start_promise;
        const std::shared_future<void> start = start_promise.get_future().share();
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;
        producers.reserve(producer_count);
        consumers.reserve(consumer_count);

        for (int consumer = 0; consumer < consumer_count; ++consumer) {
            consumers.emplace_back([&queue, &consumed, &consumed_mutex, start] {
                start.wait();
                while (true) {
                    const std::optional<int> item = queue.pop();
                    if (!item.has_value()) {
                        return;
                    }

                    std::lock_guard lock(consumed_mutex);
                    consumed.push_back(*item);
                }
            });
        }

        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&queue, start, producer] {
                start.wait();
                for (int item = 0; item < items_per_producer; ++item) {
                    const int id = producer * items_per_producer + item;
                    while (!queue.try_push(id)) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        start_promise.set_value();
        for (std::thread& producer : producers) {
            producer.join();
        }
        queue.shutdown();
        for (std::thread& consumer : consumers) {
            consumer.join();
        }

        ASSERT_EQ(consumed.size(), static_cast<std::size_t>(items_per_round));
        std::sort(consumed.begin(), consumed.end());
        for (int id = 0; id < items_per_round; ++id) {
            EXPECT_EQ(consumed[static_cast<std::size_t>(id)], id);
        }
    }
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
