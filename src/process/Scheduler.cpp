#include "process/Scheduler.h"

#include "process/PCB.h"

#include <sstream>
#include <vector>

namespace oscore {

std::string Scheduler::step(
    const std::string& owner,
    ProcessManager& processManager,
    MemoryManager& memoryManager) {
    std::ostringstream output;
    output << "=== Scheduler Step ===\n\n";

    // 每次 step 都是一次完整调度决策：先记录队列快照，再从 Q0 到 Q2 选择当前用户 READY 进程。
    const auto before = processManager.readyQueueSnapshot(owner);
    output << "[Before]\n" << before << "\n\n";

    const auto cleaned = processManager.cleanupInvalidReadyQueueEntries(owner);
    const auto selectedPid = processManager.pickNextReadyProcess(owner);
    if (!selectedPid.has_value()) {
        output << "[Select]\n"
               << "No READY process found.\n\n"
               << "[Result]\n"
               << "CPU is idle.\n\n"
               << "[After]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    auto selected = processManager.getProcessCopy(*selectedPid);
    if (!selected.has_value()) {
        output << "[Select]\n"
               << "Selected PID disappeared before running.\n\n"
               << "[Result]\n"
               << "CPU is idle.\n\n"
               << "[After]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    const int oldQueue = selected->queueLevel;
    // 时间片只由队列层级决定：Q0 最短、Q2 最长，体现多级反馈队列的响应性和吞吐折中。
    const auto quantum = static_cast<std::uint32_t>(quantumForQueue(oldQueue));
    output << "[Select]\n"
           << "Scanning Q0 -> Q1 -> Q2";
    if (!cleaned.empty()) {
        output << "\nRemoved invalid ready queue entries:";
        for (const auto pid : cleaned) {
            output << ' ' << pid;
        }
    }
    output << "\nScanning " << queueName(oldQueue) << " -> found PID=" << selected->pid << '\n'
           << "Selected PID=" << selected->pid
           << ", name=" << selected->name
           << ", queue=" << queueName(oldQueue)
           << ", quantum=" << quantum << "\n\n";

    if (!processManager.markRunning(selected->pid)) {
        output << "[Run]\n"
               << "Failed to switch PID=" << selected->pid << " to RUNNING.\n\n"
               << "[Result]\n"
               << "CPU is idle.\n\n"
               << "[After]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    output << "[Run]\n";
    std::string tickLog;
    // tickProcess 负责更新 executedTime、remainingTime 和 timeSliceLeft；Scheduler 只决定本轮最多运行多少 tick。
    if (!processManager.tickProcess(selected->pid, quantum, tickLog)) {
        (void)processManager.markReady(selected->pid);
        (void)processManager.enqueueReadyProcess(selected->pid);
        output << tickLog << "\n\n"
               << "[Result]\n"
               << "State restored to READY because tick execution failed.\n\n"
               << "[After]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }
    output << tickLog << "\n\n";

    const auto afterRun = processManager.getProcessCopy(selected->pid);
    if (!afterRun.has_value()) {
        output << "[Result]\n"
               << "PID=" << selected->pid << " disappeared after running.\n\n"
               << "[After]\n"
               << processManager.readyQueueSnapshot(owner);
        return output.str();
    }

    // 重新读取 PCB 副本，避免使用运行前快照判断完成状态。
    const auto ticksUsed = afterRun->executedTime - selected->executedTime;
    output << "[Result]\n";
    if (afterRun->remainingTime == 0) {
        std::vector<std::uint32_t> removedPids;
        std::string killMessage;
        // 完成进程按 kill 子树处理，确保父进程结束时子进程也被清理。
        processManager.killProcess(owner, selected->pid, removedPids, killMessage);

        output << "PID=" << selected->pid << " completed.\n"
               << killMessage;

        // 进程完成后必须释放物理内存；如果完成的是父进程，沿用 kill_pcb 的子树清理策略。
        for (const auto removedPid : removedPids) {
            std::string memoryMessage;
            if (memoryManager.freeByPid(owner, removedPid, memoryMessage)) {
                output << '\n' << memoryMessage;
            }
        }
    } else {
        // 未完成的进程回到 READY；如果耗尽完整时间片，则按 MLFQ 规则降级。
        const bool usedFullQuantum = ticksUsed >= quantum;
        if (usedFullQuantum) {
            // 用完整时间片仍未完成，说明该进程偏 CPU 密集，按 MLFQ 规则向低优先级队列移动。
            (void)processManager.demoteProcess(selected->pid);
        }
        (void)processManager.markReady(selected->pid);
        (void)processManager.enqueueReadyProcess(selected->pid);

        const auto finalPcb = processManager.getProcessCopy(selected->pid);
        output << "PID=" << selected->pid << " used "
               << (usedFullQuantum ? "full quantum" : "partial quantum")
               << " but not finished.\n";
        if (finalPcb.has_value()) {
            if (usedFullQuantum) {
                output << "Demote: " << queueName(oldQueue) << " -> " << queueName(finalPcb->queueLevel) << '\n';
            }
            output << "State: RUNNING -> READY";
        }
    }

    output << "\n\n[After]\n"
           << processManager.readyQueueSnapshot(owner);
    return output.str();
}

bool Scheduler::isRunning() const {
    return running_;
}

void Scheduler::setRunning(bool running) {
    running_ = running;
}

int Scheduler::quantumForQueue(int queueLevel) const {
    // Q0/Q1/Q2 的时间片与 ProcessManager 保持一致，课程演示时可直接对应 readyq 输出。
    switch (queueLevel) {
    case 0:
        return 2;
    case 1:
        return 4;
    default:
        return 8;
    }
}

std::string Scheduler::queueName(int queueLevel) const {
    return "Q" + std::to_string(queueLevel);
}

} // namespace oscore
