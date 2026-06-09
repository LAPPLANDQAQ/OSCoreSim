#include "process/ProcessManager.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace oscore {

namespace {

[[nodiscard]] std::vector<std::uint32_t> sortedIds(const std::vector<std::uint32_t>& ids) {
    auto result = ids;
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] std::string joinIds(const std::vector<std::uint32_t>& ids) {
    if (ids.empty()) {
        return "none";
    }

    std::ostringstream output;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            output << ' ';
        }
        output << ids[i];
    }
    return output.str();
}

} // namespace

// ProcessManager 只维护 PCB 表、父子进程关系和就绪队列索引；
// 进程内存的实际分配/释放由 Kernel 协调 MemoryManager 完成。
bool ProcessManager::createProcess(
    const std::string& owner,
    const std::string& name,
    std::uint32_t memKB,
    int priority,
    std::uint32_t totalTime,
    std::optional<std::uint32_t> ppid,
    std::string& message) {
    std::uint32_t ignoredPid = 0;
    return createProcessWithMemory(owner, name, memKB, 0, priority, totalTime, ppid, ignoredPid, message);
}

bool ProcessManager::createProcessWithMemory(
    const std::string& owner,
    const std::string& name,
    std::uint32_t memKB,
    std::uint32_t memStart,
    int priority,
    std::uint32_t totalTime,
    std::optional<std::uint32_t> ppid,
    std::uint32_t& outPid,
    std::string& message) {
    if (owner.empty()) {
        message = "Create failed: user must login first.";
        return false;
    }
    if (name.empty()) {
        message = "Create failed: process name cannot be empty.";
        return false;
    }
    if (memKB == 0) {
        message = "Create failed: memKB must be greater than 0.";
        return false;
    }
    if (!isValidPriority(priority)) {
        message = "Create failed: priority must be in range 0 to 15.";
        return false;
    }
    if (totalTime == 0) {
        message = "Create failed: totalTime must be greater than 0.";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t parentPid = ppid.value_or(0);
    if (parentPid != 0 && !hasOwnedProcessLocked(owner, parentPid)) {
        message = "Create failed: parent PID does not exist or access denied.";
        return false;
    }

    PCB pcb;
    pcb.pid = nextPid_++;
    pcb.ppid = parentPid;
    pcb.name = name;
    pcb.owner = owner;
    pcb.state = ProcessState::READY;
    pcb.priority = priority;
    pcb.queueLevel = queueLevelForPriority(priority);
    pcb.totalTime = totalTime;
    pcb.executedTime = 0;
    pcb.remainingTime = totalTime;
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);
    pcb.memStart = memStart;
    pcb.memSize = memKB;
    pcb.swappedOut = false;

    const std::uint32_t pid = pcb.pid;
    // pcbTable_ 是以 PID 为主键的进程控制块表，是进程查询、状态迁移和快照导出的权威数据源。
    pcbTable_.emplace(pid, std::move(pcb));
    if (parentPid != 0) {
        // children 只保存同一用户下已验证的父子关系，用于进程树展示和递归 kill。
        pcbTable_.at(parentPid).children.push_back(pid);
    }
    // READY 进程必须同步进入对应优先级队列，否则调度器无法选中它。
    enqueueReadyLocked(pid);
    outPid = pid;

    std::ostringstream output;
    output << "[OK] Process created.\n"
           << "PID=" << pid
           << ", Name=" << name
           << ", State=READY"
           << ", Priority=" << priority
           << ", Queue=" << queueName(queueLevelForPriority(priority))
           << ", Memory=" << memKB << "KB";
    message = output.str();
    return true;
}

bool ProcessManager::killProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::vector<std::uint32_t> removed;
    return killProcess(owner, pid, removed, message);
}

bool ProcessManager::killProcess(
    const std::string& owner,
    std::uint32_t pid,
    std::vector<std::uint32_t>& removedPids,
    std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasOwnedProcessLocked(owner, pid)) {
        message = "Kill failed: PID does not exist or access denied.";
        return false;
    }

    std::vector<std::uint32_t> removed;
    std::unordered_set<std::uint32_t> visited;
    // 删除进程时先收集整棵子树，保证父进程退出时不会遗留孤儿 PCB。
    collectSubtreeLocked(owner, pid, removed, visited);

    for (const auto removedPid : removed) {
        removeFromReadyQueuesLocked(removedPid);
        auto it = pcbTable_.find(removedPid);
        if (it != pcbTable_.end()) {
            it->second.state = ProcessState::TERMINATED;
        }
    }

    for (auto& [_, pcb] : pcbTable_) {
        pcb.children.erase(
            std::remove_if(pcb.children.begin(), pcb.children.end(), [&visited](std::uint32_t childPid) {
                return visited.find(childPid) != visited.end();
            }),
            pcb.children.end());
    }

    for (const auto removedPid : removed) {
        pcbTable_.erase(removedPid);
    }

    // 物理内存释放由 Kernel 在拿到 removedPids 后统一协调 MemoryManager 完成。
    std::ostringstream output;
    output << "[OK] Killed process subtree. Removed PIDs: " << joinIds(removed);
    message = output.str();
    removedPids = removed;
    return true;
}

bool ProcessManager::blockProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "Block failed: PID does not exist or access denied.";
        return false;
    }

    auto& pcb = it->second;
    if (pcb.state != ProcessState::READY && pcb.state != ProcessState::RUNNING) {
        message = "Block failed: only READY or RUNNING process can be blocked.";
        return false;
    }

    const auto oldState = toString(pcb.state);
    // 阻塞会使进程失去调度资格，因此必须从就绪队列移除后再改状态。
    removeFromReadyQueuesLocked(pid);
    pcb.state = ProcessState::BLOCKED;

    std::ostringstream output;
    output << "[OK] PID=" << pid << " blocked: " << oldState << " -> BLOCKED.";
    message = output.str();
    return true;
}

bool ProcessManager::wakeupProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "Wakeup failed: PID does not exist or access denied.";
        return false;
    }

    auto& pcb = it->second;
    if (pcb.state != ProcessState::BLOCKED) {
        message = "Wakeup failed: only BLOCKED process can be awakened.";
        return false;
    }

    // 唤醒只把 BLOCKED 进程恢复为 READY，并重新按 queueLevel 挂回就绪队列。
    pcb.state = ProcessState::READY;
    enqueueReadyLocked(pid);
    message = "[OK] PID=" + std::to_string(pid) + " awakened: BLOCKED -> READY.";
    return true;
}

bool ProcessManager::suspendProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "Suspend failed: PID does not exist or access denied.";
        return false;
    }

    auto& pcb = it->second;
    const auto oldState = toString(pcb.state);
    // 挂起保持“原来是否可运行”的语义：READY/RUNNING 进入 SUSPENDED_READY，BLOCKED 进入 SUSPENDED_BLOCKED。
    if (pcb.state == ProcessState::READY || pcb.state == ProcessState::RUNNING) {
        removeFromReadyQueuesLocked(pid);
        pcb.state = ProcessState::SUSPENDED_READY;
    } else if (pcb.state == ProcessState::BLOCKED) {
        pcb.state = ProcessState::SUSPENDED_BLOCKED;
    } else {
        message = "Suspend failed: process state cannot be suspended.";
        return false;
    }

    std::ostringstream output;
    output << "[OK] PID=" << pid << " suspended: " << oldState << " -> " << toString(pcb.state) << '.';
    message = output.str();
    return true;
}

bool ProcessManager::resumeProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "Resume failed: PID does not exist or access denied.";
        return false;
    }

    auto& pcb = it->second;
    const auto oldState = toString(pcb.state);
    // 恢复时只有 SUSPENDED_READY 会重新进入就绪队列，SUSPENDED_BLOCKED 仍保持阻塞语义。
    if (pcb.state == ProcessState::SUSPENDED_READY) {
        pcb.state = ProcessState::READY;
        enqueueReadyLocked(pid);
    } else if (pcb.state == ProcessState::SUSPENDED_BLOCKED) {
        pcb.state = ProcessState::BLOCKED;
    } else {
        message = "Resume failed: only suspended process can be resumed.";
        return false;
    }

    std::ostringstream output;
    output << "[OK] PID=" << pid << " resumed: " << oldState << " -> " << toString(pcb.state) << '.';
    message = output.str();
    return true;
}

bool ProcessManager::reniceProcess(
    const std::string& owner,
    std::uint32_t pid,
    int newPriority,
    std::string& message) {
    if (!isValidPriority(newPriority)) {
        message = "Renice failed: priority must be in range 0 to 15.";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "Renice failed: PID does not exist or access denied.";
        return false;
    }

    auto& pcb = it->second;
    const int oldPriority = pcb.priority;
    const int oldQueue = pcb.queueLevel;
    // renice 会改变队列层级；READY 进程需要先摘队再按新优先级入队，避免队列中出现重复 PID。
    if (pcb.state == ProcessState::READY) {
        removeFromReadyQueuesLocked(pid);
    }

    pcb.priority = newPriority;
    pcb.queueLevel = queueLevelForPriority(newPriority);
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);

    if (pcb.state == ProcessState::READY) {
        enqueueReadyLocked(pid);
    }

    std::ostringstream output;
    output << "[OK] PID=" << pid << " priority changed: "
           << oldPriority << '(' << queueName(oldQueue) << ") -> "
           << newPriority << '(' << queueName(pcb.queueLevel) << ')';
    message = output.str();
    return true;
}

bool ProcessManager::markSwappedOut(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "Swap out failed: PID does not exist or access denied.";
        return false;
    }
    auto& pcb = it->second;
    if (pcb.swappedOut || pcb.state == ProcessState::SWAPPED) {
        message = "Swap out failed: process is already swapped out.";
        return false;
    }

    removeFromReadyQueuesLocked(pid);
    // 换出释放物理内存后，PCB 仍保留所需内存大小，便于缺页恢复时重新申请。
    pcb.swappedOut = true;
    pcb.memStart = 0;
    pcb.state = ProcessState::SWAPPED;
    message = "[OK] PID=" + std::to_string(pid) + " marked as SWAPPED.";
    return true;
}

std::optional<std::uint32_t> ProcessManager::pickNextReadyProcess(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 调度器始终从高优先级队列开始扫描；其他用户的 READY 进程会被保留，不能被当前用户调度。
    for (auto& queue : readyQueues_) {
        for (auto it = queue.begin(); it != queue.end();) {
            const auto pid = *it;
            auto pcbIt = pcbTable_.find(pid);
            if (pcbIt == pcbTable_.end() || pcbIt->second.state != ProcessState::READY || pcbIt->second.swappedOut) {
                it = queue.erase(it);
                continue;
            }
            if (pcbIt->second.owner != owner) {
                ++it;
                continue;
            }

            queue.erase(it);
            return pid;
        }
    }

    return std::nullopt;
}

bool ProcessManager::removeFromReadyQueues(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool removed = false;
    for (auto& queue : readyQueues_) {
        const auto oldSize = queue.size();
        queue.erase(std::remove(queue.begin(), queue.end(), pid), queue.end());
        removed = removed || oldSize != queue.size();
    }
    return removed;
}

bool ProcessManager::enqueueReadyProcess(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::READY || it->second.swappedOut) {
        return false;
    }

    enqueueReadyLocked(pid);
    return true;
}

bool ProcessManager::demoteProcess(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return false;
    }

    auto& pcb = it->second;
    if (pcb.queueLevel < 2) {
        ++pcb.queueLevel;
    }
    // MLFQ 降级后重置时间片，下一次重新入队时按新队列的量程运行。
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);
    return true;
}

bool ProcessManager::markRunning(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::READY || it->second.swappedOut) {
        return false;
    }

    removeFromReadyQueuesLocked(pid);
    it->second.state = ProcessState::RUNNING;
    if (it->second.timeSliceLeft == 0) {
        it->second.timeSliceLeft = timeSliceForQueue(it->second.queueLevel);
    }
    return true;
}

bool ProcessManager::markReady(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() ||
        it->second.state == ProcessState::TERMINATED ||
        it->second.state == ProcessState::SWAPPED ||
        it->second.swappedOut) {
        return false;
    }

    it->second.state = ProcessState::READY;
    if (it->second.timeSliceLeft == 0) {
        it->second.timeSliceLeft = timeSliceForQueue(it->second.queueLevel);
    }
    return true;
}

bool ProcessManager::markTerminated(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return false;
    }

    removeFromReadyQueuesLocked(pid);
    it->second.state = ProcessState::TERMINATED;
    return true;
}

bool ProcessManager::tickProcess(std::uint32_t pid, std::uint32_t ticks, std::string& log) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::RUNNING || ticks == 0) {
        log = "Tick failed: PID does not exist or is not RUNNING.";
        return false;
    }

    auto& pcb = it->second;
    std::ostringstream output;
    // 每个 tick 只推进 1 个时间单位，便于课程演示观察执行时间和剩余时间的变化。
    for (std::uint32_t tick = 1; tick <= ticks && pcb.remainingTime > 0; ++tick) {
        ++pcb.executedTime;
        --pcb.remainingTime;
        if (pcb.timeSliceLeft > 0) {
            --pcb.timeSliceLeft;
        }
        output << "tick " << tick << '/' << ticks
               << ": PID=" << pid
               << " executed=" << pcb.executedTime << '/' << pcb.totalTime
               << ", remaining=" << pcb.remainingTime << '\n';
    }

    log = output.str();
    if (!log.empty() && log.back() == '\n') {
        log.pop_back();
    }
    return true;
}

std::optional<PCB> ProcessManager::getProcessCopy(std::uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::uint32_t> ProcessManager::cleanupInvalidReadyQueueEntries(const std::string& owner) {
    (void)owner;
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> removed;
    for (auto& queue : readyQueues_) {
        for (auto it = queue.begin(); it != queue.end();) {
            auto pcbIt = pcbTable_.find(*it);
            if (pcbIt == pcbTable_.end() || pcbIt->second.state != ProcessState::READY || pcbIt->second.swappedOut) {
                removed.push_back(*it);
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    return removed;
}

bool ProcessManager::updateProcessMemoryStart(std::uint32_t pid, std::uint32_t newStart) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return false;
    }
    it->second.memStart = newStart;
    return true;
}

bool ProcessManager::hasProcess(const std::string& owner, std::uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasOwnedProcessLocked(owner, pid);
}

bool ProcessManager::isSwappedOut(const std::string& owner, std::uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    return it != pcbTable_.end() && it->second.owner == owner && it->second.swappedOut;
}

std::vector<PCB> ProcessManager::getProcessCopiesForUser(const std::string& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PCB> copies;
    copies.reserve(pcbTable_.size());
    for (const auto& [_, pcb] : pcbTable_) {
        if (pcb.owner == owner) {
            copies.push_back(pcb);
        }
    }
    // 按 PID 升序排列，方便 overview 进程树构建
    std::sort(copies.begin(), copies.end(), [](const PCB& left, const PCB& right) {
        return left.pid < right.pid;
    });
    return copies;
}

std::uint32_t ProcessManager::nextPid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nextPid_;
}

std::uint32_t ProcessManager::timeSliceForQueueLevel(int queueLevel) {
    return timeSliceForQueue(queueLevel);
}

std::string ProcessManager::queueNameForLevel(int queueLevel) {
    return queueName(queueLevel);
}

std::string ProcessManager::showProcess(const std::string& owner, std::uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        return "Process not found or access denied.";
    }

    const auto& pcb = it->second;
    std::ostringstream output;
    output << "=== PCB Detail ===\n"
           << "PID: " << pcb.pid << '\n'
           << "PPID: " << pcb.ppid << '\n'
           << "Name: " << pcb.name << '\n'
           << "Owner: " << pcb.owner << '\n'
           << "State: " << toString(pcb.state) << '\n'
           << "Priority: " << pcb.priority << '\n'
           << "Queue: " << queueName(pcb.queueLevel) << '\n'
           << "CPU Time: " << pcb.executedTime << " / " << pcb.totalTime << '\n'
           << "Remaining: " << pcb.remainingTime << '\n'
           << "Time Slice Left: " << pcb.timeSliceLeft << '\n'
           << "Memory: ";
    if (pcb.swappedOut) {
        output << "swapped out, required=" << pcb.memSize << "KB\n";
    } else {
        output << "start=" << pcb.memStart << "KB, size=" << pcb.memSize << "KB\n";
    }
    output
           << "Swapped Out: " << (pcb.swappedOut ? "true" : "false") << '\n'
           << "Children: " << joinIds(sortedIds(pcb.children));
    return output.str();
}

std::string ProcessManager::listProcesses(const std::string& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PCB> visible;
    for (const auto& [_, pcb] : pcbTable_) {
        if (pcb.owner == owner) {
            visible.push_back(pcb);
        }
    }
    std::sort(visible.begin(), visible.end(), [](const PCB& left, const PCB& right) {
        return left.pid < right.pid;
    });

    if (visible.empty()) {
        return "No process found for current user.";
    }

    std::ostringstream output;
    output << "=== PCB List for user: " << owner << " ===\n"
           << std::left << std::setw(6) << "PID"
           << std::setw(7) << "PPID"
           << std::setw(12) << "Name"
           << std::setw(18) << "State"
           << std::setw(7) << "Prio"
           << std::setw(8) << "Queue"
           << std::setw(10) << "CPU"
           << std::setw(10) << "MemStart"
           << std::setw(8) << "MemKB"
           << "Swapped\n";

    for (const auto& pcb : visible) {
        std::ostringstream cpu;
        cpu << pcb.executedTime << '/' << pcb.totalTime;
        output << std::left << std::setw(6) << pcb.pid
               << std::setw(7) << pcb.ppid
               << std::setw(12) << pcb.name
               << std::setw(18) << toString(pcb.state)
               << std::setw(7) << pcb.priority
               << std::setw(8) << queueName(pcb.queueLevel)
               << std::setw(10) << cpu.str()
               << std::setw(10) << (pcb.swappedOut ? "-" : std::to_string(pcb.memStart))
               << std::setw(8) << pcb.memSize
               << (pcb.swappedOut ? "true" : "false") << '\n';
    }

    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

std::string ProcessManager::processTree(const std::string& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> roots;
    for (const auto& [pid, pcb] : pcbTable_) {
        if (pcb.owner != owner) {
            continue;
        }
        const auto parent = pcbTable_.find(pcb.ppid);
        if (pcb.ppid == 0 || parent == pcbTable_.end() || parent->second.owner != owner) {
            roots.push_back(pid);
        }
    }
    std::sort(roots.begin(), roots.end());

    std::ostringstream output;
    output << "Process Tree for user: " << owner;
    if (roots.empty()) {
        output << "\nNo process found for current user.";
        return output.str();
    }

    std::unordered_set<std::uint32_t> visited;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        output << '\n';
        appendTreeNodeLocked(owner, roots[i], "", i + 1 == roots.size(), true, visited, output);
    }
    return output.str();
}

std::string ProcessManager::readyQueueSnapshot(const std::string& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "Ready Queues:";
    for (std::size_t q = 0; q < readyQueues_.size(); ++q) {
        output << '\n' << queueName(static_cast<int>(q)) << ": ";
        bool first = true;
        for (const auto pid : readyQueues_[q]) {
            auto it = pcbTable_.find(pid);
            if (it == pcbTable_.end() || it->second.owner != owner || it->second.state != ProcessState::READY) {
                continue;
            }
            if (!first) {
                output << ' ';
            }
            output << pid;
            first = false;
        }
        if (first) {
            output << "empty";
        }
    }
    return output.str();
}

std::size_t ProcessManager::processCount(const std::string& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(pcbTable_.begin(), pcbTable_.end(), [&owner](const auto& item) {
        return item.second.owner == owner;
    }));
}

std::uint32_t ProcessManager::exportNextPid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nextPid_;
}

void ProcessManager::importNextPid(std::uint32_t nextPid) {
    std::lock_guard<std::mutex> lock(mutex_);
    nextPid_ = std::max<std::uint32_t>(nextPid, 1);
}

std::vector<PCB> ProcessManager::exportPcbs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PCB> pcbs;
    pcbs.reserve(pcbTable_.size());
    for (const auto& [_, pcb] : pcbTable_) {
        pcbs.push_back(pcb);
    }
    std::sort(pcbs.begin(), pcbs.end(), [](const PCB& left, const PCB& right) {
        return left.pid < right.pid;
    });
    return pcbs;
}

void ProcessManager::importPcbs(const std::vector<PCB>& pcbs) {
    std::lock_guard<std::mutex> lock(mutex_);
    pcbTable_.clear();
    for (auto& queue : readyQueues_) {
        queue.clear();
    }

    nextPid_ = 1;
    for (const auto& pcb : pcbs) {
        pcbTable_[pcb.pid] = pcb;
        nextPid_ = std::max(nextPid_, pcb.pid + 1);
    }

    for (const auto& [pid, pcb] : pcbTable_) {
        if (pcb.state == ProcessState::READY) {
            readyQueues_[static_cast<std::size_t>(pcb.queueLevel)].push_back(pid);
        }
    }
}

std::array<std::vector<std::uint32_t>, 3> ProcessManager::exportReadyQueues() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::array<std::vector<std::uint32_t>, 3> queues;
    for (std::size_t i = 0; i < readyQueues_.size(); ++i) {
        queues[i].assign(readyQueues_[i].begin(), readyQueues_[i].end());
    }
    return queues;
}

void ProcessManager::importReadyQueues(const std::array<std::vector<std::uint32_t>, 3>& queues) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < readyQueues_.size(); ++i) {
        readyQueues_[i].clear();
        readyQueues_[i].insert(readyQueues_[i].end(), queues[i].begin(), queues[i].end());
    }
}

void ProcessManager::rebuildParentChildRelationsIfNeeded() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, pcb] : pcbTable_) {
        pcb.children.clear();
    }

    // 载入快照后以 ppid 为准重建 children，避免损坏文件留下悬挂的子进程引用。
    for (const auto& [pid, pcb] : pcbTable_) {
        if (pcb.ppid == 0) {
            continue;
        }
        auto parent = pcbTable_.find(pcb.ppid);
        if (parent != pcbTable_.end() && parent->second.owner == pcb.owner) {
            parent->second.children.push_back(pid);
        }
    }

    for (auto& [_, pcb] : pcbTable_) {
        std::sort(pcb.children.begin(), pcb.children.end());
        pcb.children.erase(std::unique(pcb.children.begin(), pcb.children.end()), pcb.children.end());
    }
}

bool ProcessManager::validateReadyQueues(std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_set<std::uint32_t> seen;
    std::size_t removed = 0;
    for (std::size_t q = 0; q < readyQueues_.size(); ++q) {
        auto& queue = readyQueues_[q];
        for (auto it = queue.begin(); it != queue.end();) {
            auto pcbIt = pcbTable_.find(*it);
            const bool invalid =
                pcbIt == pcbTable_.end() ||
                pcbIt->second.state != ProcessState::READY ||
                pcbIt->second.swappedOut ||
                pcbIt->second.queueLevel != static_cast<int>(q) ||
                seen.find(*it) != seen.end();
            if (invalid) {
                it = queue.erase(it);
                ++removed;
            } else {
                seen.insert(*it);
                ++it;
            }
        }
    }

    if (removed == 0) {
        message = "Ready queue validation passed.";
    } else {
        message = "Ready queue validation repaired " + std::to_string(removed) + " invalid entries.";
    }
    return true;
}

bool ProcessManager::isValidPriority(int priority) {
    return priority >= 0 && priority <= 15;
}

int ProcessManager::queueLevelForPriority(int priority) {
    if (priority <= 3) {
        return 0;
    }
    if (priority <= 7) {
        return 1;
    }
    return 2;
}

std::uint32_t ProcessManager::timeSliceForQueue(int queueLevel) {
    switch (queueLevel) {
    case 0:
        return 2;
    case 1:
        return 4;
    default:
        return 8;
    }
}

std::string ProcessManager::queueName(int queueLevel) {
    return "Q" + std::to_string(queueLevel);
}

bool ProcessManager::hasOwnedProcessLocked(const std::string& owner, std::uint32_t pid) const {
    auto it = pcbTable_.find(pid);
    return it != pcbTable_.end() && it->second.owner == owner;
}

void ProcessManager::removeFromReadyQueuesLocked(std::uint32_t pid) {
    for (auto& queue : readyQueues_) {
        queue.erase(std::remove(queue.begin(), queue.end(), pid), queue.end());
    }
}

void ProcessManager::enqueueReadyLocked(std::uint32_t pid) {
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::READY) {
        return;
    }

    removeFromReadyQueuesLocked(pid);
    // 就绪队列只保存 PID，完整状态仍回查 pcbTable_；这样队列结构轻量且便于快照校验。
    const auto queueIndex = static_cast<std::size_t>(it->second.queueLevel);
    if (queueIndex < readyQueues_.size()) {
        readyQueues_[queueIndex].push_back(pid);
    }
}

void ProcessManager::collectSubtreeLocked(
    const std::string& owner,
    std::uint32_t pid,
    std::vector<std::uint32_t>& ordered,
    std::unordered_set<std::uint32_t>& visited) const {
    if (visited.find(pid) != visited.end()) {
        return;
    }

    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        return;
    }

    visited.insert(pid);
    ordered.push_back(pid);
    // 递归按 PID 排序遍历子进程，使 kill 输出和测试结果稳定。
    for (const auto childPid : sortedIds(it->second.children)) {
        collectSubtreeLocked(owner, childPid, ordered, visited);
    }
}

void ProcessManager::appendTreeNodeLocked(
    const std::string& owner,
    std::uint32_t pid,
    const std::string& prefix,
    bool isLast,
    bool isRoot,
    std::unordered_set<std::uint32_t>& visited,
    std::ostringstream& output) const {
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        return;
    }

    if (!isRoot) {
        output << prefix << (isLast ? "`- " : "|- ");
    }

    if (visited.find(pid) != visited.end()) {
        output << "PID=" << pid << " (cycle detected)";
        return;
    }

    visited.insert(pid);
    const auto& pcb = it->second;
    output << pcb.name << '(' << pcb.pid << ") ["
           << toString(pcb.state)
           << ", prio=" << pcb.priority
           << ", q=" << queueName(pcb.queueLevel)
           << ", mem=" << pcb.memSize << "KB]";

    std::vector<std::uint32_t> visibleChildren;
    for (const auto childPid : pcb.children) {
        auto child = pcbTable_.find(childPid);
        if (child != pcbTable_.end() && child->second.owner == owner) {
            visibleChildren.push_back(childPid);
        }
    }
    std::sort(visibleChildren.begin(), visibleChildren.end());

    const std::string childPrefix = isRoot ? "" : prefix + (isLast ? "   " : "|  ");
    for (std::size_t i = 0; i < visibleChildren.size(); ++i) {
        output << '\n';
        appendTreeNodeLocked(
            owner,
            visibleChildren[i],
            childPrefix,
            i + 1 == visibleChildren.size(),
            false,
            visited,
            output);
    }
}

} // namespace oscore
