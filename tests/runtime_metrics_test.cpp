#include <gtest/gtest.h>

#include <inferserve/backend.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/batcher.hpp>
#include <inferserve/batch_worker_pool.hpp>
#include <inferserve/request_admission.hpp>
#include <inferserve/runtime_metrics.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

class SlowMetricsBackend final : public inferserve::Backend {
public:
    void load() override {}

    [[nodiscard]] inferserve::InferenceResults infer(const inferserve::Batch& batch) override
    {
        std::this_thread::sleep_for(160ms);
        return inferserve::InferenceResults(batch.size(), inferserve::Data{1.0F});
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return 8; }
    [[nodiscard]] std::string_view name() const noexcept override { return "slow-metrics-test"; }
};

TEST(RuntimeMetrics, MeasuresQueueWaitBatchSizeExecutionAndAdmissionRejection)
{
    inferserve::RuntimeMetrics metrics;
    inferserve::RequestAdmission admission(1, &metrics);
    inferserve::BatchDispatchQueue dispatch_queue(2);
    SlowMetricsBackend backend;
    inferserve::BatchWorkerPool workers(dispatch_queue, backend, 1, &metrics);
    inferserve::Batcher batcher(admission, dispatch_queue, 8, 250ms, &metrics);

    inferserve::Request accepted(inferserve::RequestId(1), inferserve::Data{1.0F});
    auto accepted_response = accepted.get_future();
    ASSERT_EQ(admission.admit(std::move(accepted)), inferserve::Status::Success);

    inferserve::Request rejected(inferserve::RequestId(2), inferserve::Data{2.0F});
    auto rejected_response = rejected.get_future();
    EXPECT_EQ(admission.admit(std::move(rejected)), inferserve::Status::QueueFull);
    EXPECT_EQ(rejected_response.get().status, inferserve::Status::QueueFull);

    std::this_thread::sleep_for(60ms);
    workers.start();
    batcher.start();
    ASSERT_EQ(accepted_response.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(accepted_response.get().status, inferserve::Status::Success);
    batcher.stop();
    workers.shutdown();
    workers.join();

    const inferserve::RuntimeMetricsSnapshot snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.queue_wait_samples, 1U);
    EXPECT_GE(snapshot.queue_wait_total, 40ms);
    EXPECT_LT(snapshot.queue_wait_total, 200ms);
    EXPECT_EQ(snapshot.dispatched_batch_counts.size(), 1U);
    EXPECT_EQ(snapshot.dispatched_batch_counts.at(1), 1U);
    EXPECT_EQ(snapshot.execution_samples, 1U);
    EXPECT_GE(snapshot.execution_total, 140ms);
    EXPECT_EQ(snapshot.rejected_requests, 1U);
}

}  // namespace
