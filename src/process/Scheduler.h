#pragma once

#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"

#include <string>

namespace oscore {

// Scheduler：MLFQ（多级反馈队列）调度器。
// 只负责单步调度决策（step），不持有进程表或内存表。
// 自动调度线程由 Kernel 创建，反复调用 step() 并受 schedulerRunning 标志控制。
// 每次 step 完成以下操作：
//   1. 从 ProcessManager 的 3 级就绪队列中扫描（Q0→Q1→Q2），选择当前用户的首个 READY 进程
//   2. 将进程状态切换为 RUNNING，执行若干 tick
//   3. tick 用尽但未完成 → 降级到下一级队列（Q0→Q1→Q2）
//   4. 进程完成 → 递归删除 PCB 子树并释放物理内存
//   5. 进程未完成且时间片未用尽 → 回到就绪队列同级别
class Scheduler {
public:
    Scheduler() = default;

    // 执行一次完整的单步调度决策，返回格式化的调度过程日志。
    // owner: 当前登录用户，调度器只选取该用户的 READY 进程。
    // processManager / memoryManager: 由 Kernel 持有并传入，Scheduler 不拥有它们。
    [[nodiscard]] std::string step(
        const std::string& owner,
        ProcessManager& processManager,
        MemoryManager& memoryManager);

    [[nodiscard]] bool isRunning() const;   // 供 Kernel 查询自动调度线程是否活动
    void setRunning(bool running);          // 供 Kernel 启停自动调度

private:
    // 每级队列的时间片长度：Q0=2, Q1=4, Q2=8
    [[nodiscard]] int quantumForQueue(int queueLevel) const;
    // 队列名称格式化："Q0", "Q1", "Q2"
    [[nodiscard]] std::string queueName(int queueLevel) const;

    bool running_ = false;  // 自动调度线程的运行标志
};

} // namespace oscore
