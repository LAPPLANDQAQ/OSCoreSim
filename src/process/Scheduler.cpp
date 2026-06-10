#include "process/Scheduler.h"

#include "process/PCB.h"

#include <sstream>
#include <vector>

namespace oscore {

// ============================================================================
// MLFQ 单步调度算法（step）
// ============================================================================
//
// 每次 step 执行一次完整的调度决策周期，分为 5 个阶段：
//
// [1] 调度前  — 记录当前就绪队列快照，供日志对比
// [2] 选择    — 从 Q0→Q1→Q2 扫描，选取第一个属于当前用户的 READY 进程
//              清理途中遇到的无效条目（状态非 READY、已换出、队列层级不匹配等）
// [3] 执行    — 将进程标记为 RUNNING，执行至多 quantum 个 tick
//              每 tick：executedTime++  remainingTime--  timeSliceLeft--
// [4] 结果    — 判断三种结果：
//              a) remainingTime==0 → 进程完成，递归删除子树并释放内存
//              b) 用满时间片但未完成 → 降级到下一级队列（Q0→Q1→Q2），回到 READY
//              c) 未用满时间片 → 保留在原队列，回到 READY
// [5] 调度后  — 记录调度后队列快照
//
// 关键设计：
// - 时间片大小：Q0=2, Q1=4, Q2=8（高优先级短时间片 = 响应优先，低优先级长时间片 = 吞吐优先）
// - 队列降级：进程用满时间片说明是 CPU 密集型 → 向低优先级移动，给交互型进程让路
// - 用户隔离：调度器只扫描当前用户的 READY 进程，不触碰其他用户的进程
// ============================================================================

std::string Scheduler::step(
    const std::string& owner,
    ProcessManager& processManager,
    MemoryManager& memoryManager) {
    std::ostringstream output;
    // 每次 step 都返回完整日志，调用方只负责打印，不再解释调度细节。
    output << "=== 调度单步 / Scheduler Step ===\n\n";

    // ---------- [1] 调度前：记录就绪队列快照 ----------
    const auto before = processManager.readyQueueSnapshot(owner);
    // before 是调度前的只读快照，用于和调度后队列对比。
    output << "[调度前]\n" << before << "\n\n";

    // ---------- [2] 选择进程 ----------
    // 先清理无效条目（状态不为 READY、已换出、队列层级不一致等）
    const auto cleaned = processManager.cleanupInvalidReadyQueueEntries(owner);
    // pickNextReadyProcess 从 Q0 开始扫描，返回第一个属于 owner 的 READY 进程
    const auto selectedPid = processManager.pickNextReadyProcess(owner);

    // 情况 A：没有可调度的进程 → CPU 空闲
    if (!selectedPid.has_value()) {
        // 没有 READY 进程时不修改任何 PCB，仅输出 CPU 空闲。
        output << "[选择]\n"
               << "未找到 READY 进程。\n\n"
               << "[结果]\n"
               << "CPU 空闲。\n\n"
               << "[调度后]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    // 获取进程副本（快照），避免在执行过程中引用失效
    auto selected = processManager.getProcessCopy(*selectedPid);
    if (!selected.has_value()) {
        // 防御性分支：如果刚取出的 PID 已不存在，直接结束本次 step。
        output << "[选择]\n"
               << "选中的 PID 在执行前消失。\n\n"
               << "[结果]\nCPU 空闲。\n\n"
               << "[调度后]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    // 当前进程所在队列层级（降级前）
    const int oldQueue = selected->queueLevel;
    // 时间片由队列层级决定：Q0=2, Q1=4, Q2=8
    const auto quantum = static_cast<std::uint32_t>(quantumForQueue(oldQueue));

    // 记录选择过程
    output << "[选择]\n"
           << "扫描 Q0 -> Q1 -> Q2";
    if (!cleaned.empty()) {
        // cleaned 中的 PID 是调度前被剔除的陈旧就绪队列条目。
        output << "\n已移除无效就绪队列条目:";
        for (const auto pid : cleaned) output << ' ' << pid;
    }
    output << "\n扫描 " << queueName(oldQueue)
           << " -> 找到 PID=" << selected->pid << '\n'
           << "选中 PID=" << selected->pid
           << ", 名称=" << selected->name
           << ", 队列=" << queueName(oldQueue)
           << ", 时间片=" << quantum << "\n\n";

    // ---------- [3] 执行 tick ----------
    // markRunning 做三件事：从就绪队列移除 + 状态改为 RUNNING + 重置时间片（如需要）
    if (!processManager.markRunning(selected->pid)) {
        // 标记 RUNNING 失败通常说明状态已不再是 READY，本次调度放弃。
        output << "[执行]\n"
               << "切换 PID=" << selected->pid << " 到 RUNNING 失败。\n\n"
               << "[结果]\nCPU 空闲。\n\n"
               << "[调度后]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    output << "[执行]\n";
    std::string tickLog;
    // tickProcess 逐 tick 执行：executedTime++  remainingTime--  timeSliceLeft--
    // 循环提前终止条件：remainingTime 归零或 timeSliceLeft 耗尽
    if (!processManager.tickProcess(selected->pid, quantum, tickLog)) {
        // tick 执行失败 → 恢复为 READY 并重新入队
        // 这里不删除进程，尽量恢复到可调度状态并把错误写入日志。
        (void)processManager.markReady(selected->pid);
        (void)processManager.enqueueReadyProcess(selected->pid);
        output << tickLog << "\n\n"
               << "[结果]\n"
               << "tick 执行失败，状态恢复为 READY。\n\n"
               << "[调度后]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }
    output << tickLog << "\n\n";

    // ---------- [4] 判断结果 ----------
    // 重新获取进程副本以读取执行后的状态
    const auto afterRun = processManager.getProcessCopy(selected->pid);
    if (!afterRun.has_value()) {
        // 如果执行后 PCB 不存在，说明外部状态发生异常变化，本次只输出队列快照。
        output << "[结果]\n"
               << "PID=" << selected->pid << " 在执行后消失。\n\n"
               << "[调度后]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    // 实际消耗的 tick 数 = 执行后 executedTime - 执行前 executedTime
    const auto ticksUsed = afterRun->executedTime - selected->executedTime;
    // ticksUsed 用来判断是否耗尽完整时间片，不能直接使用 quantum。
    output << "[结果]\n";

    // --- 结果 A：进程完成（remainingTime == 0）---
    if (afterRun->remainingTime == 0) {
        std::vector<std::uint32_t> removedPids;
        std::string killMessage;
        // killProcess 递归删除进程子树
        // 完成父进程时，子进程也会被递归删除，符合 kill_pcb 子树语义。
        processManager.killProcess(owner, selected->pid, removedPids, killMessage);
        output << "PID=" << selected->pid << " 已完成。\n" << killMessage;
        // 释放子树中所有进程的物理内存
        for (const auto removedPid : removedPids) {
            std::string memoryMessage;
            // MemoryManager 只在找到对应 PROCESS 块时返回 true 并输出释放信息。
            if (memoryManager.freeByPid(owner, removedPid, memoryMessage)) {
                output << '\n' << memoryMessage;
            }
        }
    } else {
        // --- 结果 B/C：进程未完成 ---
        // 判断是否用满了完整时间片
        const bool usedFullQuantum = ticksUsed >= quantum;

        if (usedFullQuantum) {
            // 结果 B：用满时间片 → 降级到下一级队列（Q0→Q1, Q1→Q2, Q2 不变）
            // 这是 MLFQ 的核心机制：CPU 密集型进程逐级下移，交互型短进程留在高优先级
            (void)processManager.demoteProcess(selected->pid);
        }
        // 进程回到 READY 状态并重新入队（保持或更新后的队列层级）
        // markReady 只改状态和时间片；enqueueReadyProcess 才真正把 PID 放回队列。
        (void)processManager.markReady(selected->pid);
        (void)processManager.enqueueReadyProcess(selected->pid);

        const auto finalPcb = processManager.getProcessCopy(selected->pid);
        // finalPcb 读取降级后的队列层级，用于日志展示。
        output << "PID=" << selected->pid << " 已用 "
               << (usedFullQuantum ? "完整时间片" : "部分时间片")
               << "但未完成。\n";
        if (finalPcb.has_value() && usedFullQuantum) {
            output << "降级: " << queueName(oldQueue)
                   << " -> " << queueName(finalPcb->queueLevel) << '\n';
        }
        if (finalPcb.has_value()) {
            output << "状态: RUNNING -> READY";
        }
    }

    // ---------- [5] 调度后：记录最终队列快照 ----------
    output << "\n\n[调度后]\n"
           << processManager.readyQueueSnapshot(owner);
    return output.str();
}

bool Scheduler::isRunning() const { return running_; }

// Kernel 调用 setRunning 控制自动调度标志；step 本身不检查该标志。
void Scheduler::setRunning(bool running) { running_ = running; }

int Scheduler::quantumForQueue(int queueLevel) const {
    // Q0/Q1/Q2 的时间片与 ProcessManager 保持一致
    switch (queueLevel) {
    case 0: return 2;
    case 1: return 4;
    default: return 8;
    }
}

std::string Scheduler::queueName(int queueLevel) const {
    // 日志中统一使用 Q0/Q1/Q2 表示队列层级。
    return "Q" + std::to_string(queueLevel);
}

} // namespace oscore
