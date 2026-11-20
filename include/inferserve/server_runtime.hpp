#pragma once

#include <csignal>
#include <cstdint>
#include <memory>

#include <inferserve/backend.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/batcher.hpp>
#include <inferserve/batch_worker_pool.hpp>
#include <inferserve/http_server.hpp>
#include <inferserve/request_admission.hpp>

namespace inferserve {

// Owns the processing pipeline for the lifetime of the HTTP server.
class ServerRuntime {
public:
    ServerRuntime(Backend& backend, std::uint16_t port);

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return server_.port(); }
    void run(const volatile std::sig_atomic_t* shutdown_requested = nullptr);

private:
    void stop_processing() noexcept;

    RequestAdmission admission_{1024};
    BatchDispatchQueue dispatch_queue_{1024};
    Backend& backend_;
    HttpServer server_;
    std::unique_ptr<Batcher> batcher_;
    std::unique_ptr<BatchWorkerPool> workers_;
    bool workers_started_ = false;
    bool has_run_ = false;
};

}  // namespace inferserve
