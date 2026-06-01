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
    pcbTable_.emplace(pid, std::move(pcb));
    if (parentPid != 0) {
        pcbTable_.at(parentPid).children.push_back(pid);
    }
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
    pcb.swappedOut = true;
    pcb.memStart = 0;
    pcb.state = ProcessState::SWAPPED;
    message = "[OK] PID=" + std::to_string(pid) + " marked as SWAPPED.";
    return true;
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

std::uint32_t ProcessManager::nextPid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nextPid_;
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
