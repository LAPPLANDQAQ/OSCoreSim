#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oscore {

// BlockingQueue 是 Kernel 命令请求队列：前台线程 push，worker 线程阻塞 pop。
// shutdown 会唤醒等待线程，保证程序退出时不遗留后台线程。
template <typename T>
class BlockingQueue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdownRequested_) {
                throw std::runtime_error("cannot push to a shutdown BlockingQueue");
            }
            values_.push_back(std::move(value));
        }
        condition_.notify_one();
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 队列为空时阻塞；shutdown 后唤醒所有等待线程，便于后台线程安全退出。
        condition_.wait(lock, [this] {
            return shutdownRequested_ || !values_.empty();
        });

        if (values_.empty()) {
            return false;
        }

        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    T pop() {
        T value;
        if (!pop(value)) {
            throw std::runtime_error("BlockingQueue is shutdown");
        }
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

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdownRequested_ = true;
        }
        condition_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.clear();
        shutdownRequested_ = false;
    }

    [[nodiscard]] bool isShutdown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdownRequested_;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<T> values_;
    bool shutdownRequested_ = false;
};

} // namespace oscore
