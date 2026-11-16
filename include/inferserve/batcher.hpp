#pragma once

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

    Batcher(RequestAdmission& admission, std::size_t max_batch_size, BatchCallback callback)
        : admission_(admission), max_batch_size_(max_batch_size), callback_(std::move(callback))
    {
        if (max_batch_size_ == 0) {
            throw std::invalid_argument("batcher maximum batch size must be positive");
        }
        if (!callback_) {
            throw std::invalid_argument("batcher callback must be provided");
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
            std::optional<Request> first = admission_.pop();
            if (!first.has_value()) {
                return;
            }

            RequestBatch batch;
            batch.reserve(max_batch_size_);
            batch.push_back(std::move(*first));

            Request next(RequestId(0), Data{});
            while (batch.size() < max_batch_size_ && admission_.try_pop(next)) {
                batch.push_back(std::move(next));
                next = Request(RequestId(0), Data{});
            }

            callback_(batch);
        }
    }

    RequestAdmission& admission_;
    const std::size_t max_batch_size_;
    BatchCallback callback_;
    bool started_ = false;
    std::thread thread_;
};

}  // namespace inferserve
