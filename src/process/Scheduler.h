#pragma once

#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"

#include <string>

namespace oscore {

// Scheduler 执行一次真实 MLFQ 调度 step：选取 READY 进程、推进 tick、降级队列、完成后释放资源。
// 自动调度线程只反复调用 step，不直接绕过 ProcessManager 或 MemoryManager。
class Scheduler {
public:
    Scheduler() = default;

    [[nodiscard]] std::string step(
        const std::string& owner,
        ProcessManager& processManager,
        MemoryManager& memoryManager);

    [[nodiscard]] bool isRunning() const;
    void setRunning(bool running);

private:
    [[nodiscard]] int quantumForQueue(int queueLevel) const;
    [[nodiscard]] std::string queueName(int queueLevel) const;

    bool running_ = false;
};

} // namespace oscore
