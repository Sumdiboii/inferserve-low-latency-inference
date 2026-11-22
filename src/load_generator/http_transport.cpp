#include "http_transport.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/time.h>
#include <thread>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace inferserve::load_generator {
namespace {

constexpr std::chrono::seconds kSocketTimeout{2};
// Short enough to avoid dominating fast synthetic/ONNX observations; bounded worker
// concurrency keeps this lightweight while the 5-second deadline remains unchanged.
constexpr std::chrono::milliseconds kPollingInterval{10};
constexpr std::chrono::seconds kPollingTimeout{5};

bool connect_with_timeout(int descriptor, const sockaddr* address, socklen_t address_length)
{
    const int original_flags = ::fcntl(descriptor, F_GETFL, 0);
    if (original_flags < 0 || ::fcntl(descriptor, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        return false;
    }

    if (::connect(descriptor, address, address_length) == 0) {
        return ::fcntl(descriptor, F_SETFL, original_flags) == 0;
    }
    if (errno != EINPROGRESS) {
        return false;
    }

    pollfd event{descriptor, POLLOUT, 0};
    const auto deadline = std::chrono::steady_clock::now() + kSocketTimeout;
    int poll_result = 0;
    do {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            errno = ETIMEDOUT;
            return false;
        }
        poll_result = ::poll(&event, 1, static_cast<int>(remaining.count()));
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        errno = ETIMEDOUT;
        return false;
    }
    if (poll_result < 0) {
        return false;
    }

    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0) {
        return false;
    }
    if (socket_error != 0) {
        errno = socket_error;
        return false;
    }
    return ::fcntl(descriptor, F_SETFL, original_flags) == 0;
}

bool set_io_timeouts(int descriptor)
{
    const auto timeout = std::chrono::duration_cast<std::chrono::microseconds>(kSocketTimeout);
    timeval value{static_cast<time_t>(timeout.count() / 1'000'000),
                  static_cast<suseconds_t>(timeout.count() % 1'000'000)};
    return ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0 &&
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
}

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

std::optional<std::uint64_t> parse_request_id(std::string_view body)
{
    constexpr std::string_view key = "\"request_id\":";
    const std::size_t key_start = body.find(key);
    if (key_start == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view value = body.substr(key_start + key.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    std::uint64_t request_id = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), request_id);
    if (error != std::errc{} || end == value.data()) {
        return std::nullopt;
    }
    return request_id;
}

std::string_view response_body(std::string_view response)
{
    const std::size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string_view::npos) {
        return {};
    }
    return response.substr(body_start + 4);
}

std::optional<int> parse_http_status(std::string_view response)
{
    const std::size_t line_end = response.find("\r\n");
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view status_line = response.substr(0, line_end);
    const std::size_t status_start = status_line.find(' ');
    if (status_start == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t status_end = status_line.find(' ', status_start + 1);
    const std::string_view status_text = status_line.substr(
        status_start + 1,
        status_end == std::string_view::npos
            ? status_line.size() - status_start - 1
            : status_end - status_start - 1);
    int status = 0;
    const auto [parsed_end, error] = std::from_chars(
        status_text.data(), status_text.data() + status_text.size(), status);
    if (error != std::errc{} || parsed_end != status_text.data() + status_text.size()) {
        return std::nullopt;
    }
    return status;
}

RequestResult parse_admission_response(std::string_view response)
{
    const auto parsed_status = parse_http_status(response);
    if (!parsed_status.has_value()) {
        return failure("invalid HTTP response: malformed status line");
    }
    const int status = *parsed_status;
    if (status != 202) {
        std::string message = "HTTP response status " + std::to_string(status);
        const std::string_view body = response_body(response);
        if (!body.empty()) {
            message += ": ";
            message += body;
        }
        return failure(std::move(message), status);
    }

    const auto request_id = parse_request_id(response_body(response));
    if (!request_id.has_value()) {
        return failure("malformed HTTP 202 response: missing request_id", 202);
    }
    return {true, 202, {}, true, false};
}

std::optional<RequestResult> poll_once(std::string_view host, std::uint16_t port,
                                       std::uint64_t request_id)
{
    const std::string host_string(host);
    const std::string service = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        return failure("could not resolve HTTP host while polling: " + std::string(::gai_strerror(lookup)));
    }

    int connected_socket = -1;
    int connection_error = ECONNREFUSED;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        connected_socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connected_socket < 0) {
            connection_error = errno;
            continue;
        }
        if (connect_with_timeout(connected_socket, address->ai_addr, address->ai_addrlen)) {
            break;
        }
        connection_error = errno;
        ::close(connected_socket);
        connected_socket = -1;
    }
    ::freeaddrinfo(addresses);
    if (connected_socket < 0) {
        return failure("could not connect to HTTP server while polling: " +
                       std::string(std::strerror(connection_error)));
    }
    const Socket socket(connected_socket);
    if (!set_io_timeouts(socket.get())) {
        return failure("could not configure HTTP polling socket timeouts: " +
                       std::string(std::strerror(errno)));
    }

    const std::string request = "GET /results/" + std::to_string(request_id) +
        " HTTP/1.1\r\nHost: " + host_string +
        "\r\nConnection: close\r\n\r\n";
    std::size_t sent_total = 0;
    while (sent_total < request.size()) {
        const ssize_t sent = ::send(socket.get(), request.data() + sent_total,
                                    request.size() - sent_total, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return failure(sent < 0
                               ? "failed to send HTTP polling request: " + std::string(std::strerror(errno))
                               : "failed to send HTTP polling request: send returned zero bytes");
        }
        sent_total += static_cast<std::size_t>(sent);
    }

    std::string response;
    std::array<char, 2048> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + kSocketTimeout;
    while (true) {
        pollfd event{socket.get(), POLLIN, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return failure("timed out waiting for HTTP polling response");
        }
        const int poll_result = ::poll(&event, 1, static_cast<int>(remaining.count()));
        if (poll_result <= 0) {
            if (poll_result == 0) {
                return failure("timed out waiting for HTTP polling response");
            }
            if (errno == EINTR) {
                continue;
            }
            return failure("failed waiting for HTTP polling response: " +
                           std::string(std::strerror(errno)));
        }
        const ssize_t received = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            return failure("failed to receive HTTP polling response: " +
                           std::string(std::strerror(errno)));
        }
        if (received == 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }

    const auto parsed_status = parse_http_status(response);
    if (!parsed_status.has_value()) {
        return failure("invalid HTTP polling response: malformed status line");
    }
    const int status = *parsed_status;
    const std::string_view body = response_body(response);
    if (status == 202) {
        if (body.find("\"state\":\"pending\"") == std::string_view::npos) {
            return failure("unexpected HTTP polling response: pending state missing", 202);
        }
        return std::nullopt;
    }
    if (status == 200) {
        if (body.find("\"state\":\"terminal\"") == std::string_view::npos ||
            body.find("\"status\":\"success\"") == std::string_view::npos) {
            return failure("unexpected HTTP polling response: successful terminal state missing", 200);
        }
        return RequestResult{true, 200, {}, true, false};
    }
    if (status >= 400 && status <= 599 &&
        body.find("\"state\":\"terminal\"") != std::string_view::npos) {
        const std::string_view error_key = "\"error\":\"";
        std::string message = "terminal inference failure";
        const std::size_t error_start = body.find(error_key);
        if (error_start != std::string_view::npos) {
            const std::size_t value_start = error_start + error_key.size();
            const std::size_t value_end = body.find('"', value_start);
            if (value_end != std::string_view::npos) {
                message += ": " + std::string(body.substr(value_start, value_end - value_start));
            }
        }
        RequestResult result = failure(std::move(message), status);
        result.terminal_failure = true;
        return result;
    }
    return failure("unexpected HTTP polling response status " + std::to_string(status), status);
}

RequestResult poll_completion(std::string_view host, std::uint16_t port,
                              std::uint64_t request_id)
{
    const auto deadline = std::chrono::steady_clock::now() + kPollingTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = poll_once(host, port, request_id);
        if (result.has_value()) {
            return *result;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds::zero()) {
            break;
        }
        const auto sleep_duration = std::min(kPollingInterval,
                                              std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        std::this_thread::sleep_for(sleep_duration);
    }
    return failure("timed out polling for inference completion");
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
        if (connect_with_timeout(connected_socket, address->ai_addr, address->ai_addrlen)) {
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
    if (!set_io_timeouts(socket.get())) {
        return failure("could not configure HTTP socket timeouts: " + std::string(std::strerror(errno)));
    }

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
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return failure("timed out sending HTTP request");
            }
            const std::string reason = sent == 0 ? "send returned zero bytes" : std::strerror(errno);
            return failure("failed to send HTTP request: " + reason);
        }
        sent_total += static_cast<std::size_t>(sent);
    }

    std::string response;
    std::array<char, 2048> buffer{};
    const auto response_deadline = std::chrono::steady_clock::now() + kSocketTimeout;
    while (true) {
        pollfd event{socket.get(), POLLIN, 0};
        int poll_result = 0;
        do {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                response_deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                return failure("timed out waiting for HTTP response");
            }
            poll_result = ::poll(&event, 1, static_cast<int>(remaining.count()));
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result == 0) {
            return failure("timed out waiting for HTTP response");
        }
        if (poll_result < 0) {
            return failure("failed waiting for HTTP response: " + std::string(std::strerror(errno)));
        }
        const ssize_t received = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return failure("timed out waiting for HTTP response");
            }
            return failure("failed to receive HTTP response: " + std::string(std::strerror(errno)));
        }
        if (received == 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    const RequestResult admission = parse_admission_response(response);
    if (!admission.success || admission.http_status != 202) {
        return admission;
    }
    const auto request_id = parse_request_id(response_body(response));
    if (!request_id.has_value()) {
        return failure("malformed HTTP 202 response: missing request_id", 202);
    }
    // This callback returns only after the client observes terminal state, so the executor
    // records the polling-observed completion timestamp rather than the backend timestamp.
    return poll_completion(host_string, port, *request_id);
}

}  // namespace inferserve::load_generator
