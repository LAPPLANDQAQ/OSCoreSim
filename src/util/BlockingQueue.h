#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oscore {

template <typename T>
class BlockingQueue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                throw std::runtime_error("cannot push to a closed BlockingQueue");
            }
            values_.push_back(std::move(value));
        }
        condition_.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return closed_ || !values_.empty();
        });

        if (values_.empty()) {
            throw std::runtime_error("BlockingQueue is closed");
        }

        T value = std::move(values_.front());
        values_.pop_front();
        return value;
    }

    bool tryPop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (values_.empty()) {
            return false;
        }

        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<T> values_;
    bool closed_ = false;
};

} // namespace oscore
