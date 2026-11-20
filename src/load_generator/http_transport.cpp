#include "http_transport.hpp"

#include <array>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace inferserve::load_generator {
namespace {

class Socket {
public:
    explicit Socket(int descriptor) : descriptor_(descriptor) {}
    ~Socket()
    {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

RequestResult failure(std::string message, int status = 0)
{
    return {false, status, std::move(message)};
}

RequestResult parse_response(std::string_view response)
{
    const std::size_t line_end = response.find("\r\n");
    if (line_end == std::string_view::npos) {
        return failure("invalid HTTP response: missing status line");
    }
    const std::string_view status_line = response.substr(0, line_end);
    const std::size_t status_start = status_line.find(' ');
    if (status_start == std::string_view::npos) {
        return failure("invalid HTTP response: missing status code");
    }
    const std::size_t status_end = status_line.find(' ', status_start + 1);
    const std::string_view status_text = status_line.substr(
        status_start + 1,
        status_end == std::string_view::npos ? status_line.size() - status_start - 1
                                             : status_end - status_start - 1);
    int status = 0;
    const auto [parsed_end, error] = std::from_chars(status_text.data(), status_text.data() + status_text.size(), status);
    if (error != std::errc{} || parsed_end != status_text.data() + status_text.size()) {
        return failure("invalid HTTP response: malformed status code");
    }
    if (status >= 200 && status < 300) {
        return {true, status, {}};
    }

    std::string message = "HTTP response status " + std::to_string(status);
    const std::size_t body_start = response.find("\r\n\r\n");
    if (body_start != std::string_view::npos && body_start + 4 < response.size()) {
        message += ": ";
        message += response.substr(body_start + 4);
    }
    return failure(std::move(message), status);
}

}  // namespace

RequestResult send_infer_request(std::string_view host, std::uint16_t port)
{
    const std::string host_string(host);
    const std::string service = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        return failure("could not resolve HTTP host: " + std::string(::gai_strerror(lookup)));
    }

    int connected_socket = -1;
    int connection_error = ECONNREFUSED;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        connected_socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connected_socket < 0) {
            connection_error = errno;
            continue;
        }
        if (::connect(connected_socket, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        connection_error = errno;
        ::close(connected_socket);
        connected_socket = -1;
    }
    ::freeaddrinfo(addresses);
    if (connected_socket < 0) {
        return failure("could not connect to HTTP server: " + std::string(std::strerror(connection_error)));
    }
    const Socket socket(connected_socket);

    constexpr std::string_view body = "{\"input\":[1,2]}";
    const std::string request = "POST /infer HTTP/1.1\r\nHost: " + host_string +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + std::string(body);
    std::size_t sent_total = 0;
    while (sent_total < request.size()) {
        const ssize_t sent = ::send(socket.get(), request.data() + sent_total, request.size() - sent_total, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            const std::string reason = sent == 0 ? "send returned zero bytes" : std::strerror(errno);
            return failure("failed to send HTTP request: " + reason);
        }
        sent_total += static_cast<std::size_t>(sent);
    }

    std::string response;
    std::array<char, 2048> buffer{};
    while (true) {
        const ssize_t received = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            return failure("failed to receive HTTP response: " + std::string(std::strerror(errno)));
        }
        if (received == 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    return parse_response(response);
}

}  // namespace inferserve::load_generator
