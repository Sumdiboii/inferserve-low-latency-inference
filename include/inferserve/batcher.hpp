#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <inferserve/backend.hpp>
#include <inferserve/batch_dispatch_queue.hpp>
#include <inferserve/core_types.hpp>
#include <inferserve/request_admission.hpp>

namespace inferserve {

class Batcher {
public:
    using RequestBatch = BatchDispatchQueue::RequestBatch;
    using BatchCallback = std::function<void(const RequestBatch&)>;

    Batcher(RequestAdmission& admission,
            Backend& backend,
            std::size_t max_batch_size,
            std::chrono::milliseconds max_batch_delay = std::chrono::milliseconds::zero())
        : admission_(admission),
          backend_(&backend),
          max_batch_size_(max_batch_size),
          max_batch_delay_(max_batch_delay)
    {
        validate_configuration();
    }

    Batcher(RequestAdmission& admission,
            Backend& backend,
            std::size_t max_batch_size,
            BatchCallback callback,
            std::chrono::milliseconds max_batch_delay = std::chrono::milliseconds::zero())
        : admission_(admission),
          backend_(&backend),
          max_batch_size_(max_batch_size),
          callback_(std::move(callback)),
          max_batch_delay_(max_batch_delay)
    {
        validate_configuration();
    }

    Batcher(RequestAdmission& admission,
            std::size_t max_batch_size,
            BatchCallback callback,
            std::chrono::milliseconds max_batch_delay = std::chrono::milliseconds::zero())
        : admission_(admission),
          backend_(nullptr),
          max_batch_size_(max_batch_size),
          callback_(std::move(callback)),
          max_batch_delay_(max_batch_delay)
    {
        validate_configuration();
        if (!callback_) {
            throw std::invalid_argument("batcher callback must be provided");
        }
    }

    Batcher(RequestAdmission& admission,
            BatchDispatchQueue& dispatch_queue,
            std::size_t max_batch_size,
            std::chrono::milliseconds max_batch_delay = std::chrono::milliseconds::zero())
        : admission_(admission),
          backend_(nullptr),
          max_batch_size_(max_batch_size),
          dispatch_queue_(&dispatch_queue),
          max_batch_delay_(max_batch_delay)
    {
        validate_configuration();
    }

    Batcher(const Batcher&) = delete;
    Batcher& operator=(const Batcher&) = delete;

    ~Batcher() { stop(); }

    void start()
    {
        if (started_) {
            throw std::logic_error("batcher cannot be started more than once");
        }

        started_ = true;
        thread_ = std::thread(&Batcher::run, this);
    }

    void stop()
    {
        admission_.shutdown();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run()
    {
        while (true) {
            std::optional<Request> first;
            if (pending_request_.has_value()) {
                first = std::move(*pending_request_);
                pending_request_.reset();
            } else {
                first = admission_.pop();
            }
            if (!first.has_value()) {
                return;
            }

            RequestBatch batch;
            batch.reserve(max_batch_size_);
            batch.push_back(std::move(*first));

            if (max_batch_delay_ == std::chrono::milliseconds::zero()) {
                Request next(RequestId(0), Data{});
                while (batch.size() < max_batch_size_ && admission_.try_pop(next)) {
                    if (next.input.size() != batch.front().input.size()) {
                        pending_request_ = std::move(next);
                        break;
                    }
                    batch.push_back(std::move(next));
                    next = Request(RequestId(0), Data{});
                }
            } else {
                const auto deadline = std::chrono::steady_clock::now() + max_batch_delay_;
                while (batch.size() < max_batch_size_) {
                    const auto remaining = deadline - std::chrono::steady_clock::now();
                    if (remaining <= std::chrono::steady_clock::duration::zero()) {
                        break;
                    }

                    std::optional<Request> next = admission_.pop_for(remaining);
                    if (!next.has_value()) {
                        break;
                    }
                    if (next->input.size() != batch.front().input.size()) {
                        pending_request_ = std::move(next);
                        break;
                    }
                    batch.push_back(std::move(*next));
                }
            }

            const TimePoint batch_formed = clock_.now();
            for (Request& request : batch) {
                request.batch_formed = batch_formed;
            }
            execute(batch);
        }
    }

    void validate_configuration() const
    {
        if (max_batch_size_ == 0) {
            throw std::invalid_argument("batcher maximum batch size must be positive");
        }
        if (max_batch_delay_ < std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("batcher maximum delay cannot be negative");
        }
    }

    void execute(RequestBatch& batch)
    {
        if (dispatch_queue_ != nullptr) {
            const BatchDispatchQueue::PushResult result =
                dispatch_queue_->try_push_result(std::move(batch));
            if (result != BatchDispatchQueue::PushResult::Pushed) {
                complete_with_status(
                    batch, result == BatchDispatchQueue::PushResult::Full ? Status::QueueFull
                                                                          : Status::InternalError);
            }
            return;
        }

        if (backend_ == nullptr) {
            notify_batch(batch);
            return;
        }

        Batch inputs;
        inputs.reserve(batch.size());
        for (const Request& request : batch) {
            inputs.push_back(request.input);
        }

        const TimePoint exec_start = clock_.now();
        for (Request& request : batch) {
            request.exec_start = exec_start;
        }

        try {
            const InferenceResults results = backend_->infer(inputs);
            record_exec_end(batch);
            if (results.size() != batch.size()) {
                complete_with_error(batch);
                notify_batch(batch);
                return;
            }

            for (std::size_t index = 0; index < batch.size(); ++index) {
                batch[index].completed = clock_.now();
                batch[index].complete(Response(batch[index].id, Status::Success, results[index]));
            }
            notify_batch(batch);
        } catch (...) {
            record_exec_end(batch);
            complete_with_error(batch);
            notify_batch(batch);
        }
    }

    void notify_batch(const RequestBatch& batch) noexcept
    {
        if (callback_) {
            try {
                callback_(batch);
            } catch (...) {
                // Observer failures must not change request results or stop batching.
            }
        }
    }

    void record_exec_end(RequestBatch& batch)
    {
        const TimePoint exec_end = clock_.now();
        for (Request& request : batch) {
            request.exec_end = exec_end;
        }
    }

    void complete_with_error(RequestBatch& batch)
    {
        complete_with_status(batch, Status::BackendError);
    }

    void complete_with_status(RequestBatch& batch, Status status)
    {
        for (Request& request : batch) {
            request.completed = clock_.now();
            request.complete(Response(request.id, status));
        }
    }

    RequestAdmission& admission_;
    Backend* backend_;
    const std::size_t max_batch_size_;
    BatchCallback callback_;
    BatchDispatchQueue* dispatch_queue_ = nullptr;
    const std::chrono::milliseconds max_batch_delay_;
    MonotonicClock clock_;
    std::optional<Request> pending_request_;
    bool started_ = false;
    std::thread thread_;
};

}  // namespace inferserve
