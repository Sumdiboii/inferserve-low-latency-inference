#include <gtest/gtest.h>

#include <inferserve/http_server.hpp>

#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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

}  // namespace
