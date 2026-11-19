#include <gtest/gtest.h>

#include <inferserve/http_server.hpp>

#include <chrono>
#include <csignal>
#include <future>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

void request_shutdown(int)
{
    shutdown_requested = 1;
}

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
