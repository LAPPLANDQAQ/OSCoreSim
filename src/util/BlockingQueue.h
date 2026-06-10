#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oscore {

// BlockingQueue：线程安全阻塞队列。
//
// 用于 Kernel 的命令请求队列：
//   - 前台线程（ConsoleApp / NamedPipeServer）通过 push() 提交命令
//   - 后台 worker 线程通过 pop() 阻塞等待命令
//   - shutdown() 唤醒所有等待线程，保证程序退出时不遗留后台线程
//
// 模板参数 T：队列元素类型（本项目中使用 CommandRequest）。
template <typename T>
class BlockingQueue {
public:
    // 向队尾添加元素。
    // 如果队列已 shutdown，抛出 runtime_error。
    void push(T value) {
        {
            // 加锁保护 values_ 和 shutdownRequested_。
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdownRequested_) {
                // shutdown 后拒绝新元素，防止 Kernel 停止过程中继续提交命令。
                throw std::runtime_error("cannot push to a shutdown BlockingQueue");
            }
            // 元素移动到队尾，避免不必要复制。
            values_.push_back(std::move(value));
        }
        condition_.notify_one();  // 唤醒一个等待线程（worker）
    }

    // 阻塞获取队首元素。
    // 队列为空时阻塞在 condition_variable 上，直到有元素或 shutdown。
    // 返回 false 表示队列已关闭且无可用元素。
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        // wait 谓词可防止虚假唤醒：只有 shutdown 或队列非空才继续。
        condition_.wait(lock, [this] {
            return shutdownRequested_ || !values_.empty();
        });

        if (values_.empty()) {
            // shutdown 且没有剩余元素，通知 worker 可以退出。
            return false;  // shutdown 且队列空
        }

        // 取出队首元素并移动给调用方。
        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    // 阻塞 pop 的便捷版本。shutdown 后抛异常。
    T pop() {
        T value;
        if (!pop(value)) {
            throw std::runtime_error("BlockingQueue is shutdown");
        }
        return value;
    }

    // 非阻塞尝试获取。队列空时返回 false，不会阻塞。
    bool tryPop(T& value) {
        // 非阻塞版本只检查当前状态，不等待 condition_variable。
        std::lock_guard<std::mutex> lock(mutex_);
        if (values_.empty()) return false;
        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    // 关闭队列：设置 shutdown 标志，唤醒所有等待线程。
    // worker 线程检测到 shutdown 后退出循环，实现安全关闭。
    void shutdown() {
        {
            // 写关闭标志时持锁，保证等待线程看到一致状态。
            std::lock_guard<std::mutex> lock(mutex_);
            shutdownRequested_ = true;
        }
        condition_.notify_all();  // 唤醒所有等待线程
    }

    // 重置队列到初始状态（清空元素 + 清除 shutdown 标志）。
    // 用于 Kernel::start() 时重新初始化。
    void reset() {
        // reset 用于重新启动 Kernel 前清空旧命令并重新允许 push。
        std::lock_guard<std::mutex> lock(mutex_);
        values_.clear();
        shutdownRequested_ = false;
    }

    [[nodiscard]] bool isShutdown() const {
        // 只读查询仍持锁，避免和 shutdown/reset 并发。
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdownRequested_;
    }

    [[nodiscard]] bool empty() const {
        // 查询队列是否为空，主要用于测试或诊断。
        std::lock_guard<std::mutex> lock(mutex_);
        return values_.empty();
    }

private:
    mutable std::mutex mutex_;            // 保护队列内容和关闭标志。
    std::condition_variable condition_;   // 用于阻塞/唤醒 worker 线程
    std::deque<T> values_;               // 底层双向队列，支持高效的队头弹出
    bool shutdownRequested_ = false;     // 关闭标志：true 后不允许 push，pop 返回空
};

} // namespace oscore
