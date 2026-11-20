#include <inferserve/http_server.hpp>

#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace inferserve {
namespace {

constexpr std::size_t kMaxRequestHeaderSize = 16 * 1024;
constexpr std::size_t kMaxRequestBodySize = 1024 * 1024;
constexpr std::size_t kClientWorkerCount = 4;

void skip_whitespace(std::string_view text, std::size_t& position)
{
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\r' || text[position] == '\n')) {
        ++position;
    }
}

std::optional<Data> parse_input(std::string_view body)
{
    std::size_t position = 0;
    skip_whitespace(body, position);
    if (position >= body.size() || body[position++] != '{') {
        return std::nullopt;
    }
    skip_whitespace(body, position);
    constexpr std::string_view key = "\"input\"";
    if (body.substr(position, key.size()) != key) {
        return std::nullopt;
    }
    position += key.size();
    skip_whitespace(body, position);
    if (position >= body.size() || body[position++] != ':') {
        return std::nullopt;
    }
    skip_whitespace(body, position);
    if (position >= body.size() || body[position++] != '[') {
        return std::nullopt;
    }

    Data input;
    skip_whitespace(body, position);
    while (position < body.size() && body[position] != ']') {
        float value = 0.0F;
        const char* begin = body.data() + position;
        const char* end = body.data() + body.size();
        const auto [parsed_end, error] = std::from_chars(begin, end, value);
        if (error != std::errc{} || parsed_end == begin || !std::isfinite(value)) {
            return std::nullopt;
        }
        position = static_cast<std::size_t>(parsed_end - body.data());
        input.push_back(value);
        skip_whitespace(body, position);
        if (position < body.size() && body[position] == ',') {
            ++position;
            skip_whitespace(body, position);
            if (position >= body.size() || body[position] == ']') {
                return std::nullopt;
            }
        } else {
            break;
        }
    }

    if (position >= body.size() || body[position++] != ']') {
        return std::nullopt;
    }
    skip_whitespace(body, position);
    if (position >= body.size() || body[position++] != '}') {
        return std::nullopt;
    }
    skip_whitespace(body, position);
    if (position != body.size()) {
        return std::nullopt;
    }
    return input;
}

std::optional<std::size_t> content_length(std::string_view headers)
{
    std::size_t line_start = headers.find("\r\n");
    if (line_start == std::string_view::npos) {
        return std::nullopt;
    }
    line_start += 2;
    while (line_start < headers.size()) {
        const std::size_t line_end = headers.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end == line_start) {
            break;
        }
        const std::string_view line = headers.substr(line_start, line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string name(line.substr(0, colon));
            for (char& character : name) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            if (name == "content-length") {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                std::size_t length = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
                if (error == std::errc::result_out_of_range && end == value.data() + value.size()) {
                    return std::numeric_limits<std::size_t>::max();
                }
                if (error != std::errc{} || end != value.data() + value.size()) {
                    return std::nullopt;
                }
                return length;
            }
        }
        line_start = line_end + 2;
    }
    return std::size_t{0};
}

std::string response(int status, std::string_view reason, std::string_view body)
{
    return "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
           "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

bool receive_more(int client,
                  std::array<char, 2048>& buffer,
                  std::string& request,
                  std::size_t max_bytes_to_receive,
                  const std::atomic<bool>& stopping,
                  const volatile std::sig_atomic_t* shutdown_requested)
{
    if (max_bytes_to_receive == 0) {
        return false;
    }
    while (!stopping.load() && (shutdown_requested == nullptr || *shutdown_requested == 0)) {
        pollfd connection{client, POLLIN, 0};
        const int ready = ::poll(&connection, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            continue;
        }
        if ((connection.revents & POLLIN) == 0) {
            return false;
        }

        const std::size_t receive_size = max_bytes_to_receive < buffer.size()
            ? max_bytes_to_receive
            : buffer.size();
        const ssize_t received = ::recv(client, buffer.data(), receive_size, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
        return true;
    }
    return false;
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
        if (sent == 0) {
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

void handle_client(int client,
                   RequestAdmission& admission,
                   std::atomic<std::uint64_t>& next_request_id,
                   const std::atomic<bool>& stopping,
                   const volatile std::sig_atomic_t* shutdown_requested)
{
    std::string request;
    std::array<char, 2048> buffer{};
    while (request.size() < kMaxRequestHeaderSize && request.find("\r\n\r\n") == std::string::npos) {
        if (!receive_more(client, buffer, request, kMaxRequestHeaderSize - request.size(), stopping,
                          shutdown_requested)) {
            return;
        }
    }

    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        send_all(client, response(400, "Bad Request", "{\"error\":\"bad request\"}"));
        return;
    }
    const auto line_end = request.find("\r\n");
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
        const auto body_length = content_length(std::string_view(request.data(), header_end + 2));
        if (!body_length.has_value()) {
            send_all(client, response(400, "Bad Request", "{\"error\":\"invalid request body\"}"));
            return;
        }
        if (*body_length > kMaxRequestBodySize) {
            send_all(client, response(413, "Payload Too Large", "{\"error\":\"request body is too large\"}"));
            return;
        }
        const std::size_t body_start = header_end + 4;
        while (request.size() - body_start < *body_length) {
            const std::size_t body_bytes_received = request.size() - body_start;
            const std::size_t body_bytes_remaining = *body_length - body_bytes_received;
            if (!receive_more(client, buffer, request, body_bytes_remaining, stopping, shutdown_requested)) {
                if (stopping.load() ||
                    (shutdown_requested != nullptr && *shutdown_requested != 0)) {
                    return;
                }
                send_all(client, response(400, "Bad Request", "{\"error\":\"incomplete request body\"}"));
                return;
            }
        }

        auto input = parse_input(std::string_view(request.data() + body_start, *body_length));
        if (!input.has_value()) {
            send_all(client, response(400, "Bad Request", "{\"error\":\"invalid inference input\"}"));
            return;
        }

        const std::uint64_t request_id = next_request_id.fetch_add(1);
        const Status status = admission.admit(Request(RequestId(request_id), std::move(*input)));
        if (status == Status::Success) {
            send_all(client, response(202, "Accepted", "{\"request_id\":" + std::to_string(request_id) + "}"));
        } else if (status == Status::InvalidInput) {
            send_all(client, response(400, "Bad Request", "{\"error\":\"invalid inference input\"}"));
        } else {
            send_all(client, response(503, "Service Unavailable", "{\"error\":\"request was not accepted\"}"));
        }
    } else {
        send_all(client, response(404, "Not Found", "{\"error\":\"not found\"}"));
    }
}

}  // namespace

HttpServer::HttpServer(RequestAdmission& admission, std::uint16_t port) : admission_(admission)
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

void HttpServer::run(const volatile std::sig_atomic_t* shutdown_requested)
{
    std::vector<std::thread> client_workers;
    client_workers.reserve(kClientWorkerCount);
    try {
        for (std::size_t index = 0; index < kClientWorkerCount; ++index) {
            client_workers.emplace_back([this, shutdown_requested] {
                while (std::optional<int> client = clients_.pop()) {
                    handle_client(*client, admission_, next_request_id_, stopping_, shutdown_requested);
                    ::close(*client);
                }
            });
        }
    } catch (...) {
        stop();
        clients_.shutdown();
        for (std::thread& worker : client_workers) {
            worker.join();
        }
        throw;
    }

    try {
        while (!stopping_.load()) {
            if (shutdown_requested != nullptr && *shutdown_requested != 0) {
                stop();
                break;
            }

            pollfd listener{socket_, POLLIN, 0};
            const int ready = ::poll(&listener, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (stopping_.load()) {
                    break;
                }
                throw std::runtime_error("HTTP poll failed: " + std::string(std::strerror(errno)));
            }
            if (ready == 0) {
                continue;
            }
            if (stopping_.load()) {
                break;
            }

            const int client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (stopping_.load()) {
                    break;
                }
                throw std::runtime_error("HTTP accept failed: " + std::string(std::strerror(errno)));
            }

            if (!clients_.try_push(client)) {
                send_all(client, response(503, "Service Unavailable", "{\"error\":\"server is busy\"}"));
                ::close(client);
            }
        }
    } catch (...) {
        stop();
        clients_.shutdown();
        for (std::thread& worker : client_workers) {
            worker.join();
        }
        throw;
    }

    clients_.shutdown();
    for (std::thread& worker : client_workers) {
        worker.join();
    }
}

void HttpServer::stop() noexcept
{
    if (!stopping_.exchange(true) && socket_ >= 0) {
        ::shutdown(socket_, SHUT_RDWR);
    }
}

}  // namespace inferserve
