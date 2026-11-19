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

TEST(HttpServer, RegistersHealthAndInferenceEndpoints)
{
    inferserve::HttpServer server(0);
    std::thread server_thread([&server] { server.run(); });

    const std::string health = get_response(server.port(), "GET /health?check=1 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(health.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(health.find("{\"status\":\"ok\"}"), std::string::npos);

    const std::string inference = get_response(
        server.port(), "POST /infer HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    EXPECT_NE(inference.find("HTTP/1.1 501 Not Implemented"), std::string::npos);

    server.stop();
    server_thread.join();
}

}  // namespace
