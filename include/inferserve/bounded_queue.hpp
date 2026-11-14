#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
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
            if (queue_.size() >= capacity_) {
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

    [[nodiscard]] T pop()
    {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty(); });

        T value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};

}  // namespace inferserve
