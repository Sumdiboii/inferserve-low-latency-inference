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
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>
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

class FailingBackend final : public inferserve::Backend {
public:
    void load() override {}
    [[nodiscard]] inferserve::InferenceResults infer(const inferserve::Batch&) override
    {
        throw std::runtime_error("backend failed");
    }
    [[nodiscard]] std::size_t max_batch_size() const override { return 8; }
    [[nodiscard]] std::string_view name() const noexcept override { return "failing"; }
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
    EXPECT_EQ(open_records.front().result.http_status, 200);
    EXPECT_TRUE(closed_records.front().result.success) << closed_records.front().result.error;
    EXPECT_EQ(closed_records.front().result.http_status, 200);
    EXPECT_TRUE(backend_processed_requests);
    EXPECT_LE(open_records.front().scheduled_arrival, open_records.front().actual_send);
    EXPECT_LE(open_records.front().actual_send, open_records.front().completion);
    EXPECT_LE(closed_records.front().scheduled_arrival, closed_records.front().actual_send);
    EXPECT_LE(closed_records.front().actual_send, closed_records.front().completion);
}

TEST(HttpTransport, RecordsTerminalInferenceFailure)
{
    FailingBackend backend;
    inferserve::ServerRuntime runtime(backend, 0);
    volatile std::sig_atomic_t shutdown_requested = 0;
    std::thread server_thread([&] { runtime.run(&shutdown_requested); });

    const auto result = inferserve::load_generator::send_infer_request(
        "127.0.0.1", runtime.port());
    shutdown_requested = 1;
    server_thread.join();

    EXPECT_FALSE(result.success) << result.error;
    EXPECT_EQ(result.http_status, 502) << result.error;
    EXPECT_TRUE(result.terminal_failure) << result.error;
    EXPECT_NE(result.error.find("terminal inference failure"), std::string::npos) << result.error;
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

TEST(HttpTransport, RecordsMalformedAdmissionResponse)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_family = AF_INET;
    address.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t address_length = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length), 0);
    const std::uint16_t port = ntohs(address.sin_port);

    std::thread server([listener] {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client >= 0) {
            char buffer[512];
            ::recv(client, buffer, sizeof(buffer), 0);
            const char response[] =
                "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\n"
                "Content-Length: 2\r\nConnection: close\r\n\r\n{}";
            ::send(client, response, sizeof(response) - 1, 0);
            ::close(client);
        }
        ::close(listener);
    });

    const auto result = inferserve::load_generator::send_infer_request("127.0.0.1", port);
    server.join();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 202);
    EXPECT_NE(result.error.find("missing request_id"), std::string::npos);
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

TEST(HttpTransport, TimesOutPollingPendingCompletion)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_family = AF_INET;
    address.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 16), 0);
    socklen_t address_length = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length), 0);
    const std::uint16_t port = ntohs(address.sin_port);

    std::thread server([listener] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        int index = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd event{listener, POLLIN, 0};
            const int ready = ::poll(&event, 1, 100);
            if (ready <= 0) {
                continue;
            }
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            char buffer[1024];
            ::recv(client, buffer, sizeof(buffer), 0);
            if (index == 0) {
                const char response[] =
                    "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\n"
                    "Content-Length: 19\r\nConnection: close\r\n\r\n{\"request_id\":1}";
                ::send(client, response, sizeof(response) - 1, 0);
            } else {
                const char response[] =
                    "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\n"
                    "Content-Length: 21\r\nConnection: close\r\n\r\n{\"state\":\"pending\"}";
                ::send(client, response, sizeof(response) - 1, 0);
            }
            ::close(client);
            ++index;
        }
        ::close(listener);
    });

    const auto started = std::chrono::steady_clock::now();
    const auto result = inferserve::load_generator::send_infer_request("127.0.0.1", port);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    server.join();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 0);
    EXPECT_NE(result.error.find("timed out polling"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::seconds(5));
    EXPECT_LT(elapsed, std::chrono::seconds(8));
}

TEST(HttpTransport, TimesOutWhenServerDoesNotRespond)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);

    socklen_t address_length = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length), 0);
    const auto port = ntohs(address.sin_port);

    std::thread server([listener] {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client >= 0) {
            char buffer[256];
            while (::recv(client, buffer, sizeof(buffer), 0) > 0) {
            }
            ::close(client);
        }
        ::close(listener);
    });

    const auto started = std::chrono::steady_clock::now();
    const auto result = inferserve::load_generator::send_infer_request("127.0.0.1", port);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    server.join();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 0);
    EXPECT_NE(result.error.find("timed out waiting for HTTP response"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::seconds(1));
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

}  // namespace
