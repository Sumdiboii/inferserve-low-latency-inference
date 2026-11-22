#include <inferserve/server_runtime.hpp>

#include <inferserve/batcher.hpp>
#include <inferserve/batch_worker_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace inferserve {
namespace {

constexpr std::size_t kRuntimeMaxBatchSize = 32;

}  // namespace

ServerRuntime::ServerRuntime(Backend& backend, std::uint16_t port)
    : backend_(backend), server_(admission_, port, &completion_registry_)
{
}

void ServerRuntime::run(const volatile std::sig_atomic_t* shutdown_requested)
{
    if (has_run_) {
        throw std::logic_error("server runtime cannot be run more than once");
    }
    has_run_ = true;

    backend_.load();
    const std::size_t max_batch_size = std::min(backend_.max_batch_size(), kRuntimeMaxBatchSize);
    batcher_ = std::make_unique<Batcher>(admission_, dispatch_queue_, max_batch_size,
                                         std::chrono::milliseconds::zero(), &metrics_,
                                         &completion_registry_);
    workers_ = std::make_unique<BatchWorkerPool>(
        dispatch_queue_, backend_, 1, &metrics_, &completion_registry_);
    workers_->start();
    workers_started_ = true;

    try {
        batcher_->start();
        server_.run(shutdown_requested);
    } catch (...) {
        stop_processing();
        throw;
    }
    stop_processing();
}

void ServerRuntime::stop_processing() noexcept
{
    if (batcher_) {
        batcher_->stop();
    } else {
        admission_.shutdown();
    }
    if (workers_started_) {
        workers_->shutdown();
        workers_->join();
        workers_started_ = false;
    }
}

}  // namespace inferserve
