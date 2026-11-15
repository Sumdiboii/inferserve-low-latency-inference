#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace inferserve {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    template <typename U>
    [[nodiscard]] bool try_push(U&& value)
    {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_ || queue_.size() >= capacity_) {
                return false;
            }

            queue_.push(std::forward<U>(value));
        }

        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] bool try_pop(T& value)
    {
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                return false;
            }

            value = std::move(queue_.front());
            queue_.pop();
        }

        not_full_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> pop()
    {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout)
    {
        std::unique_lock lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout, [this] { return shutdown_ || !queue_.empty(); }) ||
            queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    void shutdown()
    {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }

        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    std::queue<T> queue_;
    bool shutdown_ = false;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};

}  // namespace inferserve
