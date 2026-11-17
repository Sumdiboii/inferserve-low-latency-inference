#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <inferserve/core_types.hpp>
#include <inferserve/request_admission.hpp>

namespace inferserve {

class Batcher {
public:
    using RequestBatch = std::vector<Request>;
    using BatchCallback = std::function<void(const RequestBatch&)>;

    Batcher(RequestAdmission& admission,
            std::size_t max_batch_size,
            BatchCallback callback,
            std::chrono::milliseconds max_batch_delay = std::chrono::milliseconds::zero())
        : admission_(admission),
          max_batch_size_(max_batch_size),
          callback_(std::move(callback)),
          max_batch_delay_(max_batch_delay)
    {
        if (max_batch_size_ == 0) {
            throw std::invalid_argument("batcher maximum batch size must be positive");
        }
        if (!callback_) {
            throw std::invalid_argument("batcher callback must be provided");
        }
        if (max_batch_delay_ < std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("batcher maximum delay cannot be negative");
        }
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

            callback_(batch);
        }
    }

    RequestAdmission& admission_;
    const std::size_t max_batch_size_;
    BatchCallback callback_;
    const std::chrono::milliseconds max_batch_delay_;
    std::optional<Request> pending_request_;
    bool started_ = false;
    std::thread thread_;
};

}  // namespace inferserve
