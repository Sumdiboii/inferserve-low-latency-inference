#include <gtest/gtest.h>

#include <inferserve/backend.hpp>
#include <inferserve/batcher.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/batch_worker_pool.hpp>
#include <inferserve/http_server.hpp>
#include <inferserve/onnx_runtime_backend.hpp>
#include <inferserve/server_runtime.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

void request_shutdown(int)
{
    shutdown_requested = 1;
}

class RecordingOnnxBackend final : public inferserve::Backend {
public:
    explicit RecordingOnnxBackend(std::string model_path) : backend_(std::move(model_path)) {}

    void load() override { backend_.load(); }

    [[nodiscard]] inferserve::InferenceResults infer(const inferserve::Batch& batch) override
    {
        inferserve::InferenceResults results = backend_.infer(batch);
        result_promise_.set_value(results);
        return results;
    }

    [[nodiscard]] std::size_t max_batch_size() const override { return backend_.max_batch_size(); }
    [[nodiscard]] std::string_view name() const noexcept override { return backend_.name(); }
    [[nodiscard]] std::future<inferserve::InferenceResults> result() { return result_promise_.get_future(); }

private:
    inferserve::OnnxRuntimeBackend backend_;
    std::promise<inferserve::InferenceResults> result_promise_;
};

std::string get_response(std::uint16_t port, const std::string& request)
{
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(socket, 0);
    if (socket < 0) {
        return {};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(socket);
        ADD_FAILURE() << "could not connect to HTTP server";
        return {};
    }

    const ssize_t sent = ::send(socket, request.data(), request.size(), 0);
    EXPECT_EQ(sent, static_cast<ssize_t>(request.size()));
    std::string response;
    char buffer[1024];
    ssize_t received = 0;
    while ((received = ::recv(socket, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket);
    return response;
}

std::string post_infer(std::uint16_t port, const std::string& body)
{
    return get_response(port,
        "POST /infer HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body);
}

TEST(HttpServer, RegistersHealthAndInferenceEndpoints)
{
    inferserve::RequestAdmission admission(1);
    inferserve::HttpServer server(admission, 0);
    std::thread server_thread([&server] { server.run(); });

    const std::string health = get_response(server.port(), "GET /health?check=1 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(health.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(health.find("{\"status\":\"ok\"}"), std::string::npos);

    const std::string body = "{\"input\":[1.5,-2,3]}";
    const std::string inference = post_infer(server.port(), body);
    EXPECT_NE(inference.find("HTTP/1.1 202 Accepted"), std::string::npos);

    const std::string invalid = post_infer(server.port(), "{\"input\":[]}");
    EXPECT_NE(invalid.find("HTTP/1.1 400 Bad Request"), std::string::npos);
    EXPECT_NE(invalid.find("invalid inference input"), std::string::npos);

    const std::string full = post_infer(server.port(), body);
    EXPECT_NE(full.find("HTTP/1.1 503 Service Unavailable"), std::string::npos);
    EXPECT_NE(full.find("request was not accepted"), std::string::npos);

    inferserve::Request queued(inferserve::RequestId(0), {});
    const bool was_queued = admission.try_pop(queued);
    inferserve::Request rejected(inferserve::RequestId(0), {});
    const bool rejection_was_queued = admission.try_pop(rejected);
    server.stop();
    server_thread.join();

    ASSERT_TRUE(was_queued);
    EXPECT_EQ(queued.id, inferserve::RequestId(1));
    EXPECT_EQ(queued.input, (inferserve::Data{1.5F, -2.0F, 3.0F}));
    EXPECT_TRUE(queued.enqueued.has_value());
    EXPECT_FALSE(rejection_was_queued);
}

TEST(HttpServer, RejectsRequestBodyLargerThanOneMiB)
{
    inferserve::RequestAdmission admission(1);
    inferserve::HttpServer server(admission, 0);
    std::thread server_thread([&server] { server.run(); });

    constexpr std::size_t oversized_body_length = 1024 * 1024 + 1;
    const std::string request =
        "POST /infer HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(oversized_body_length) + "\r\n\r\n";
    const std::string response = get_response(server.port(), request);
    server.stop();
    server_thread.join();

    EXPECT_NE(response.find("HTTP/1.1 413 Payload Too Large"), std::string::npos);
}

TEST(HttpServer, ReportsAdmissionShutdownSeparatelyFromQueueSaturation)
{
    inferserve::RequestAdmission admission(1);
    admission.shutdown();
    inferserve::HttpServer server(admission, 0);
    std::thread server_thread([&server] { server.run(); });

    const std::string response = post_infer(server.port(), "{\"input\":[1.5,-2,3]}");
    server.stop();
    server_thread.join();

    EXPECT_NE(response.find("HTTP/1.1 503 Service Unavailable"), std::string::npos);
    EXPECT_NE(response.find("server is shutting down"), std::string::npos);
    EXPECT_EQ(response.find("request was not accepted"), std::string::npos);
}

TEST(HttpServer, StalledClientDoesNotBlockFollowingClient)
{
    inferserve::RequestAdmission admission(2);
    inferserve::HttpServer server(admission, 0);
    std::thread server_thread([&server] { server.run(); });

    const int stalled_client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (stalled_client < 0) {
        server.stop();
        server_thread.join();
        FAIL() << "could not create stalled client socket";
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(server.port());
    if (::connect(stalled_client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(stalled_client);
        server.stop();
        server_thread.join();
        FAIL() << "could not connect stalled client";
        return;
    }

    const std::string partial_request = "GET /health HTTP/1.1\r\nHost: localhost\r\n";
    if (::send(stalled_client, partial_request.data(), partial_request.size(), 0) !=
        static_cast<ssize_t>(partial_request.size())) {
        ::close(stalled_client);
        server.stop();
        server_thread.join();
        FAIL() << "could not send partial request";
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int valid_client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (valid_client < 0) {
        server.stop();
        ::close(stalled_client);
        server_thread.join();
        FAIL() << "could not create valid client socket";
        return;
    }
    if (::connect(valid_client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(valid_client);
        server.stop();
        ::close(stalled_client);
        server_thread.join();
        FAIL() << "could not connect valid client";
        return;
    }

    const std::string health_request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const ssize_t sent = ::send(valid_client, health_request.data(), health_request.size(), 0);
    pollfd response_ready{valid_client, POLLIN, 0};
    const int ready = sent == static_cast<ssize_t>(health_request.size())
        ? ::poll(&response_ready, 1, 1000)
        : -1;
    std::string response;
    if (ready > 0) {
        char buffer[512];
        ssize_t received = 0;
        while ((received = ::recv(valid_client, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, static_cast<std::size_t>(received));
        }
    }

    ::close(valid_client);
    server.stop();
    ::close(stalled_client);
    server_thread.join();

    EXPECT_EQ(sent, static_cast<ssize_t>(health_request.size()));
    EXPECT_GT(ready, 0);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

TEST(HttpServer, EndToEndInferenceRunsThroughOnnxBackend)
{
    RecordingOnnxBackend backend(INFERSERVE_TEST_DYNAMIC_BATCH_MODEL_PATH);
    std::future<inferserve::InferenceResults> inference_result = backend.result();
    volatile std::sig_atomic_t stop_requested = 0;
    inferserve::ServerRuntime server(backend, 0);
    std::thread server_thread([&] { server.run(&stop_requested); });

    const std::string http_response = post_infer(server.port(), "{\"input\":[1.5,-2.25]}");
    const bool inference_completed = inference_result.wait_for(std::chrono::seconds(2)) ==
                                     std::future_status::ready;
    inferserve::InferenceResults outputs;
    if (inference_completed) {
        outputs = inference_result.get();
    }

    stop_requested = 1;
    server_thread.join();

    EXPECT_NE(http_response.find("HTTP/1.1 202 Accepted"), std::string::npos);
    EXPECT_NE(http_response.find("{\"request_id\":1}"), std::string::npos);
    ASSERT_TRUE(inference_completed);
    ASSERT_EQ(outputs.size(), 1U);
    EXPECT_EQ(outputs.front(), (inferserve::Data{1.5F, -2.25F}));
}

TEST(HttpServer, SignalFlagStopsServerWhileClientRequestIsIncomplete)
{
    const auto previous_handler = std::signal(SIGUSR1, request_shutdown);
    ASSERT_NE(previous_handler, SIG_ERR);
    shutdown_requested = 0;

    inferserve::RequestAdmission admission(1);
    inferserve::HttpServer server(admission, 0);
    std::promise<void> server_stopped;
    std::future<void> stopped = server_stopped.get_future();
    std::thread server_thread([&] {
        server.run(&shutdown_requested);
        server_stopped.set_value();
    });

    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        server.stop();
        server_thread.join();
        std::signal(SIGUSR1, previous_handler);
        FAIL() << "could not create test client socket";
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(server.port());
    if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(client);
        server.stop();
        server_thread.join();
        std::signal(SIGUSR1, previous_handler);
        FAIL() << "could not connect to HTTP server";
        return;
    }

    const std::string partial_request =
        "POST /infer HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20\r\n\r\n";
    const ssize_t sent = ::send(client, partial_request.data(), partial_request.size(), 0);
    if (sent != static_cast<ssize_t>(partial_request.size())) {
        ::close(client);
        server.stop();
        server_thread.join();
        std::signal(SIGUSR1, previous_handler);
        FAIL() << "could not send partial HTTP request";
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int signal_result = ::pthread_kill(server_thread.native_handle(), SIGUSR1);

    const bool exited_on_signal = stopped.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!exited_on_signal) {
        server.stop();
    }
    ::shutdown(client, SHUT_RDWR);
    ::close(client);
    server_thread.join();
    std::signal(SIGUSR1, previous_handler);

    EXPECT_EQ(signal_result, 0);
    EXPECT_TRUE(exited_on_signal);
}

}  // namespace
