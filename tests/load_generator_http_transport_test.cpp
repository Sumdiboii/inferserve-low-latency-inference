#include <gtest/gtest.h>

#include "closed_loop_executor.hpp"
#include "http_transport.hpp"
#include "open_loop_executor.hpp"

#include <inferserve/backend.hpp>
#include <inferserve/http_server.hpp>
#include <inferserve/request_admission.hpp>
#include <inferserve/server_runtime.hpp>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class RecordingBackend final : public inferserve::Backend {
public:
    void load() override {}

    [[nodiscard]] inferserve::InferenceResults infer(const inferserve::Batch& batch) override
    {
        {
            std::lock_guard lock(mutex_);
            inference_count_ += batch.size();
        }
        condition_.notify_all();
        return std::vector<inferserve::Data>(batch.size(), inferserve::Data{1.0F});
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return 8; }
    [[nodiscard]] std::string_view name() const noexcept override { return "recording"; }

    [[nodiscard]] bool wait_for_inferences(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [&] { return inference_count_ >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t inference_count_ = 0;
};

TEST(HttpTransport, SendsOpenAndClosedLoopRequestsThroughServerRuntime)
{
    RecordingBackend backend;
    inferserve::ServerRuntime runtime(backend, 0);
    volatile std::sig_atomic_t shutdown_requested = 0;
    std::thread server_thread([&] { runtime.run(&shutdown_requested); });

    const auto send_request = [&](std::size_t, double) {
        return inferserve::load_generator::send_infer_request("127.0.0.1", runtime.port());
    };
    const auto open_records = inferserve::load_generator::execute_open_loop_with_results({0.0}, send_request);
    const auto closed_records = inferserve::load_generator::execute_closed_loop_with_results(1, send_request);

    const bool backend_processed_requests = backend.wait_for_inferences(2);
    shutdown_requested = 1;
    server_thread.join();

    ASSERT_EQ(open_records.size(), 1);
    ASSERT_EQ(closed_records.size(), 1);
    EXPECT_TRUE(open_records.front().result.success) << open_records.front().result.error;
    EXPECT_EQ(open_records.front().result.http_status, 202);
    EXPECT_TRUE(closed_records.front().result.success) << closed_records.front().result.error;
    EXPECT_EQ(closed_records.front().result.http_status, 202);
    EXPECT_TRUE(backend_processed_requests);
    EXPECT_LE(open_records.front().scheduled_arrival, open_records.front().actual_send);
    EXPECT_LE(open_records.front().actual_send, open_records.front().completion);
    EXPECT_LE(closed_records.front().scheduled_arrival, closed_records.front().actual_send);
    EXPECT_LE(closed_records.front().actual_send, closed_records.front().completion);
}

TEST(HttpTransport, RecordsRejectedHttpResponseAsFailure)
{
    inferserve::RequestAdmission admission(1);
    admission.shutdown();
    inferserve::HttpServer server(admission, 0);
    std::thread server_thread([&] { server.run(); });

    const auto result = inferserve::load_generator::send_infer_request("127.0.0.1", server.port());
    server.stop();
    server_thread.join();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 503);
    EXPECT_NE(result.error.find("server is shutting down"), std::string::npos);
}

TEST(HttpTransport, RecordsConnectionFailure)
{
    std::uint16_t closed_port = 0;
    {
        inferserve::RequestAdmission admission(1);
        inferserve::HttpServer server(admission, 0);
        closed_port = server.port();
    }
    const auto result = inferserve::load_generator::send_infer_request("127.0.0.1", closed_port);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 0);
    EXPECT_NE(result.error.find("could not connect to HTTP server"), std::string::npos);
}

}  // namespace
