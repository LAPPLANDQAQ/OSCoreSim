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

    const auto ticksUsed = afterRun->executedTime - selected->executedTime;
    output << "[Result]\n";
    if (afterRun->remainingTime == 0) {
        std::vector<std::uint32_t> removedPids;
        std::string killMessage;
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
        const bool usedFullQuantum = ticksUsed >= quantum;
        if (usedFullQuantum) {
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
