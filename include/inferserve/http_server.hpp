#pragma once

#include <atomic>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <thread>
#include <vector>

#include <inferserve/bounded_queue.hpp>
#include <inferserve/request_admission.hpp>

namespace inferserve {

class HttpServer {
public:
    HttpServer(RequestAdmission& admission, std::uint16_t port);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    void run(const volatile std::sig_atomic_t* shutdown_requested = nullptr);
    void stop() noexcept;

private:
    int socket_ = -1;
    std::uint16_t port_ = 0;
    RequestAdmission& admission_;
    BoundedQueue<int> clients_{64};
    std::atomic<std::uint64_t> next_request_id_{1};
    std::atomic<bool> stopping_{false};
};

}  // namespace inferserve
