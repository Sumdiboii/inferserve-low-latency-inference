#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <inferserve/backend.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/core_types.hpp>
#include <inferserve/runtime_metrics.hpp>
#include <inferserve/completion_registry.hpp>

namespace inferserve {

class BatchWorkerPool {
public:
    BatchWorkerPool(
        BatchDispatchQueue& dispatch_queue,
        Backend& backend,
        std::size_t worker_count,
        RuntimeMetrics* metrics = nullptr,
        CompletionRegistry* completion_registry = nullptr)
        : dispatch_queue_(dispatch_queue), backend_(backend), worker_count_(worker_count),
          metrics_(metrics), completion_registry_(completion_registry)
    {
        if (worker_count_ == 0) {
            throw std::invalid_argument("worker count must be positive");
        }
    }

    BatchWorkerPool(const BatchWorkerPool&) = delete;
    BatchWorkerPool& operator=(const BatchWorkerPool&) = delete;

    ~BatchWorkerPool()
    {
        if (!workers_.empty()) {
            shutdown();
            join();
        }
    }

    void start()
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (!workers_.empty()) {
            throw std::logic_error("worker pool cannot be started more than once");
        }
        if (shutdown_) {
            throw std::logic_error("worker pool cannot be started after shutdown");
        }

        workers_.reserve(worker_count_);
        for (std::size_t index = 0; index < worker_count_; ++index) {
            workers_.emplace_back(&BatchWorkerPool::run, this);
        }
    }

    // Call after start() to stop dispatch and let workers drain accepted batches.
    void shutdown()
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (workers_.empty()) {
            throw std::logic_error("worker pool cannot be shut down before it is started");
        }
        if (!shutdown_) {
            shutdown_ = true;
            dispatch_queue_.shutdown();
        }
    }

    void join()
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (workers_.empty() || !shutdown_) {
            throw std::logic_error("worker pool must be shut down before it is joined");
        }
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    void run()
    {
        while (std::optional<BatchDispatchQueue::RequestBatch> batch = dispatch_queue_.pop()) {
            Batch inputs;
            inputs.reserve(batch->size());
            for (const Request& request : *batch) {
                inputs.push_back(request.input);
            }

            const TimePoint exec_start = clock_.now();
            for (Request& request : *batch) {
                request.exec_start = exec_start;
            }

            try {
                const InferenceResults results = backend_.infer(inputs);
                record_exec_end(*batch, exec_start);
                if (results.size() != batch->size()) {
                    complete_with_error(*batch);
                    continue;
                }

                for (std::size_t index = 0; index < batch->size(); ++index) {
                    (*batch)[index].completed = clock_.now();
                    (*batch)[index].complete(
                        Response((*batch)[index].id, Status::Success, results[index]));
                    if (completion_registry_ != nullptr) {
                        completion_registry_->record_terminal((*batch)[index].id, Status::Success);
                    }
                }
            } catch (...) {
                record_exec_end(*batch, exec_start);
                complete_with_error(*batch);
            }
        }
    }

    void record_exec_end(BatchDispatchQueue::RequestBatch& batch, TimePoint exec_start)
    {
        const TimePoint exec_end = clock_.now();
        for (Request& request : batch) {
            request.exec_end = exec_end;
        }
        if (metrics_ != nullptr) {
            metrics_->record_execution(
                std::chrono::duration_cast<std::chrono::nanoseconds>(exec_end - exec_start));
        }
    }

    void complete_with_error(BatchDispatchQueue::RequestBatch& batch)
    {
        for (Request& request : batch) {
            request.completed = clock_.now();
            request.complete(Response(request.id, Status::BackendError));
            if (completion_registry_ != nullptr) {
                completion_registry_->record_terminal(
                    request.id, Status::BackendError, "backend error");
            }
        }
    }

    BatchDispatchQueue& dispatch_queue_;
    Backend& backend_;
    const std::size_t worker_count_;
    RuntimeMetrics* metrics_;
    CompletionRegistry* completion_registry_;
    MonotonicClock clock_;
    std::vector<std::thread> workers_;
    std::mutex lifecycle_mutex_;
    bool shutdown_ = false;
};

}  // namespace inferserve
