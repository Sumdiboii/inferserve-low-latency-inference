#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <inferserve/backend.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/core_types.hpp>

namespace inferserve {

class BatchWorkerPool {
public:
    BatchWorkerPool(BatchDispatchQueue& dispatch_queue, Backend& backend, std::size_t worker_count)
        : dispatch_queue_(dispatch_queue), backend_(backend), worker_count_(worker_count)
    {
        if (worker_count_ == 0) {
            throw std::invalid_argument("worker count must be positive");
        }
    }

    BatchWorkerPool(const BatchWorkerPool&) = delete;
    BatchWorkerPool& operator=(const BatchWorkerPool&) = delete;

    void start()
    {
        if (!workers_.empty()) {
            throw std::logic_error("worker pool cannot be started more than once");
        }

        workers_.reserve(worker_count_);
        for (std::size_t index = 0; index < worker_count_; ++index) {
            workers_.emplace_back(&BatchWorkerPool::run, this);
        }
    }

    void join()
    {
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
                record_exec_end(*batch);
                if (results.size() != batch->size()) {
                    complete_with_error(*batch);
                    continue;
                }

                for (std::size_t index = 0; index < batch->size(); ++index) {
                    (*batch)[index].completed = clock_.now();
                    (*batch)[index].complete(
                        Response((*batch)[index].id, Status::Success, results[index]));
                }
            } catch (...) {
                record_exec_end(*batch);
                complete_with_error(*batch);
            }
        }
    }

    void record_exec_end(BatchDispatchQueue::RequestBatch& batch)
    {
        const TimePoint exec_end = clock_.now();
        for (Request& request : batch) {
            request.exec_end = exec_end;
        }
    }

    void complete_with_error(BatchDispatchQueue::RequestBatch& batch)
    {
        for (Request& request : batch) {
            request.completed = clock_.now();
            request.complete(Response(request.id, Status::BackendError));
        }
    }

    BatchDispatchQueue& dispatch_queue_;
    Backend& backend_;
    const std::size_t worker_count_;
    MonotonicClock clock_;
    std::vector<std::thread> workers_;
};

}  // namespace inferserve
