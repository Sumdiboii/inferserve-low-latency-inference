#pragma once

#include <atomic>
#include <cstdint>

namespace inferserve {

class HttpServer {
public:
    explicit HttpServer(std::uint16_t port);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    void run();
    void stop() noexcept;

private:
    int socket_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
};

}  // namespace inferserve
