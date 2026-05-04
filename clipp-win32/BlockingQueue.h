#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

template <typename T>
class BlockingQueue {
public:
    void Push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Waits until an item is available OR the timeout expires.
    // Returns std::nullopt if the timeout hit before an item arrived.
    std::optional<T> WaitFor(std::chrono::milliseconds timeout, const std::atomic<bool>& stopRequested) {
        std::unique_lock<std::mutex> lock(mutex_);

        bool wokeUp = cv_.wait_for(lock, timeout, [this, &stopRequested]() {
            return !queue_.empty() || stopRequested.load();
            });

        if (stopRequested.load() || !wokeUp || queue_.empty()) {
            return std::nullopt;
        }

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    void WakeAll() {
        cv_.notify_all();
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
