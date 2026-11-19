#include <inferserve/http_server.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace inferserve {
namespace {

constexpr std::size_t kMaxRequestHeaderSize = 16 * 1024;

std::string response(int status, std::string_view reason, std::string_view body)
{
    return "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
           "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

void send_all(int socket, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t sent = ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

void handle_client(int client)
{
    std::string request;
    std::array<char, 2048> buffer{};
    while (request.size() < kMaxRequestHeaderSize && request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t received = ::recv(client, buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
    }

    const auto line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
        send_all(client, response(400, "Bad Request", "{\"error\":\"bad request\"}"));
        return;
    }
    const std::string_view request_line(request.data(), line_end);
    const auto method_end = request_line.find(' ');
    const auto target_end = method_end == std::string_view::npos
                                ? std::string_view::npos
                                : request_line.find(' ', method_end + 1);
    if (method_end == std::string_view::npos || target_end == std::string_view::npos) {
        send_all(client, response(400, "Bad Request", "{\"error\":\"bad request\"}"));
        return;
    }

    const std::string_view method = request_line.substr(0, method_end);
    const std::string_view target = request_line.substr(method_end + 1, target_end - method_end - 1);
    const std::string_view version = request_line.substr(target_end + 1);
    const auto query_start = target.find('?');
    const std::string_view path = target.substr(0, query_start);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        send_all(client, response(400, "Bad Request", "{\"error\":\"bad request\"}"));
    } else if (method == "GET" && path == "/health") {
        send_all(client, response(200, "OK", "{\"status\":\"ok\"}"));
    } else if (method == "POST" && path == "/infer") {
        send_all(client, response(501, "Not Implemented", "{\"error\":\"inference handling not implemented\"}"));
    } else {
        send_all(client, response(404, "Not Found", "{\"error\":\"not found\"}"));
    }
}

}  // namespace

HttpServer::HttpServer(std::uint16_t port)
{
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        throw std::runtime_error("failed to create HTTP socket: " + std::string(std::strerror(errno)));
    }

    const int reuse_address = 1;
    if (::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        const std::string error = std::strerror(errno);
        ::close(socket_);
        socket_ = -1;
        throw std::runtime_error("failed to configure HTTP socket: " + error);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(socket_, SOMAXCONN) < 0) {
        const std::string error = std::strerror(errno);
        ::close(socket_);
        socket_ = -1;
        throw std::runtime_error("failed to start HTTP server: " + error);
    }

    socklen_t address_size = sizeof(address);
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        const std::string error = std::strerror(errno);
        ::close(socket_);
        socket_ = -1;
        throw std::runtime_error("failed to read HTTP server port: " + error);
    }
    port_ = ntohs(address.sin_port);
}

HttpServer::~HttpServer()
{
    stop();
    if (socket_ >= 0) {
        ::close(socket_);
    }
}

void HttpServer::run()
{
    while (!stopping_.load()) {
        const int client = ::accept(socket_, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stopping_.load()) {
                return;
            }
            throw std::runtime_error("HTTP accept failed: " + std::string(std::strerror(errno)));
        }
        handle_client(client);
        ::close(client);
    }
}

void HttpServer::stop() noexcept
{
    if (!stopping_.exchange(true) && socket_ >= 0) {
        ::shutdown(socket_, SHUT_RDWR);
    }
}

}  // namespace inferserve
