#include "process/ProcessManager.h"

#include "util/StringUtil.h"

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
        return "无";
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

// createProcessWithMemory：创建进程并分配内存。
// 流程：
//   1. 校验参数（owner 非空、名称非空、memKB>0、优先级 0-15、总时间>0）
//   2. 校验父进程存在性（如果指定了 ppid）
//   3. 分配 PID（全局递增，不回收复用）
//   4. 构造 PCB 结构体（状态=READY，计算队列层级和初始时间片）
//   5. 添加到 pcbTable_（PID → PCB 映射）
//   6. 如果是子进程，将 PID 添加到父进程的 children 列表
//   7. 入队到对应层级的就绪队列（enqueueReadyLocked）
// 注意：内存已在调用方（Kernel）通过 MemoryManager 分配完成，此处只接收 memStart。
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
        message = "[失败] 创建失败：请先登录。";
        return false;
    }
    if (name.empty()) {
        message = "[失败] 创建失败：进程名不能为空。";
        return false;
    }
    if (memKB == 0) {
        message = "[失败] 创建失败：内存大小必须大于 0 KB。";
        return false;
    }
    if (!isValidPriority(priority)) {
        message = "[失败] 创建失败：优先级范围为 0-15。";
        return false;
    }
    if (totalTime == 0) {
        message = "[失败] 创建失败：总时间必须大于 0。";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t parentPid = ppid.value_or(0);
    if (parentPid != 0 && !hasOwnedProcessLocked(owner, parentPid)) {
        message = "[失败] 创建失败：父 PID 不存在或访问被拒绝。";
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
    output << "[成功] 进程创建成功。\n"
           << "PID=" << pid
           << ", 名称=" << name
           << ", 状态=READY"
           << ", 优先级=" << priority
           << ", 队列=" << queueName(queueLevelForPriority(priority))
           << ", 内存=" << memKB << "KB";
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
        message = "[失败] 删除失败：PID 不存在或访问被拒绝。";
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

    std::ostringstream output;
    output << "[成功] 删除进程子树。已移除 PID：" << joinIds(removed);
    message = output.str();
    removedPids = removed;
    return true;
}

bool ProcessManager::blockProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 阻塞失败：PID 不存在或访问被拒绝。";
        return false;
    }

    auto& pcb = it->second;
    if (pcb.state != ProcessState::READY && pcb.state != ProcessState::RUNNING) {
        message = "[失败] 阻塞失败：只能阻塞 READY 或 RUNNING 状态的进程。";
        return false;
    }

    const auto oldState = toString(pcb.state);
    removeFromReadyQueuesLocked(pid);
    pcb.state = ProcessState::BLOCKED;

    std::ostringstream output;
    output << "[成功] PID=" << pid << " 已阻塞：" << oldState << " -> BLOCKED。";
    message = output.str();
    return true;
}

bool ProcessManager::wakeupProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 唤醒失败：PID 不存在或访问被拒绝。";
        return false;
    }

    auto& pcb = it->second;
    if (pcb.state != ProcessState::BLOCKED) {
        message = "[失败] 唤醒失败：只能唤醒 BLOCKED 状态的进程。";
        return false;
    }

    pcb.state = ProcessState::READY;
    enqueueReadyLocked(pid);
    message = "[成功] PID=" + std::to_string(pid) + " 已唤醒：BLOCKED -> READY。";
    return true;
}

bool ProcessManager::suspendProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 挂起失败：PID 不存在或访问被拒绝。";
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
        message = "[失败] 挂起失败：该进程状态不能挂起。";
        return false;
    }

    std::ostringstream output;
    output << "[成功] PID=" << pid << " 已挂起：" << oldState << " -> " << toString(pcb.state) << "。";
    message = output.str();
    return true;
}

bool ProcessManager::resumeProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 恢复失败：PID 不存在或访问被拒绝。";
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
        message = "[失败] 恢复失败：只能恢复挂起状态的进程。";
        return false;
    }

    std::ostringstream output;
    output << "[成功] PID=" << pid << " 已恢复：" << oldState << " -> " << toString(pcb.state) << "。";
    message = output.str();
    return true;
}

bool ProcessManager::reniceProcess(
    const std::string& owner,
    std::uint32_t pid,
    int newPriority,
    std::string& message) {
    if (!isValidPriority(newPriority)) {
        message = "[失败] 修改优先级失败：优先级范围为 0-15。";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 修改优先级失败：PID 不存在或访问被拒绝。";
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
    output << "[成功] PID=" << pid << " 优先级已修改："
           << oldPriority << '(' << queueName(oldQueue) << ") -> "
           << newPriority << '(' << queueName(pcb.queueLevel) << ')';
    message = output.str();
    return true;
}

bool ProcessManager::markSwappedOut(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 换出失败：PID 不存在或访问被拒绝。";
        return false;
    }
    auto& pcb = it->second;
    if (pcb.swappedOut || pcb.state == ProcessState::SWAPPED) {
        message = "[失败] 换出失败：该进程已被换出。";
        return false;
    }

    removeFromReadyQueuesLocked(pid);
    pcb.swappedOut = true;
    pcb.memStart = 0;
    pcb.state = ProcessState::SWAPPED;
    message = "[成功] PID=" + std::to_string(pid) + " 已标记为 SWAPPED。";
    return true;
}

std::optional<std::uint32_t> ProcessManager::pickNextReadyProcess(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);

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
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);
    return true;
}

// markRunning：READY → RUNNING。
// 从就绪队列移除 → 状态改为 RUNNING → 确保 timeSliceLeft 非零（必要时重置为队列默认值）。
// Scheduler::step 在选中进程后调用此方法。
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
        log = "[失败] Tick 失败：PID 不存在或不是 RUNNING 状态。";
        return false;
    }

    auto& pcb = it->second;
    std::ostringstream output;
    for (std::uint32_t tick = 1; tick <= ticks && pcb.remainingTime > 0; ++tick) {
        ++pcb.executedTime;
        --pcb.remainingTime;
        if (pcb.timeSliceLeft > 0) {
            --pcb.timeSliceLeft;
        }
        output << "tick " << tick << '/' << ticks
               << ": PID=" << pid
               << " 执行=" << pcb.executedTime << '/' << pcb.totalTime
               << ", 剩余=" << pcb.remainingTime << '\n';
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
        return "[失败] 进程不存在或访问被拒绝。";
    }

    const auto& pcb = it->second;
    std::ostringstream output;
    output << "=== PCB 详情 ===\n"
           << "PID: " << pcb.pid << '\n'
           << "PPID: " << pcb.ppid << '\n'
           << "名称: " << pcb.name << '\n'
           << "所有者: " << pcb.owner << '\n'
           << "状态: " << toString(pcb.state) << '\n'
           << "优先级: " << pcb.priority << '\n'
           << "队列: " << queueName(pcb.queueLevel) << '\n'
           << "CPU 时间: " << pcb.executedTime << " / " << pcb.totalTime << '\n'
           << "剩余时间: " << pcb.remainingTime << '\n'
           << "剩余时间片: " << pcb.timeSliceLeft << '\n'
           << "内存: ";
    if (pcb.swappedOut) {
        output << "已换出，需求=" << pcb.memSize << "KB\n";
    } else {
        output << "起始=" << pcb.memStart << "KB, 大小=" << pcb.memSize << "KB\n";
    }
    output
           << "是否换出: " << (pcb.swappedOut ? "是" : "否") << '\n'
           << "子进程: " << joinIds(sortedIds(pcb.children));
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
        return "当前用户没有进程。";
    }

    std::ostringstream output;
    output << "=== 进程列表 / PCB List [用户: " << owner << "] ===\n"
           << std::left
           << padRightDisplayWidth("PID", 6)
           << padRightDisplayWidth("PPID", 6)
           << padRightDisplayWidth("Name", 14)
           << padRightDisplayWidth("State", 18)
           << padRightDisplayWidth("Prio", 6)
           << padRightDisplayWidth("Queue", 7)
           << padRightDisplayWidth("CPU", 10)
           << padRightDisplayWidth("MemStart", 10)
           << padRightDisplayWidth("MemKB", 8)
           << padRightDisplayWidth("Swap", 6)
           << '\n';

    for (const auto& pcb : visible) {
        std::ostringstream cpu;
        cpu << pcb.executedTime << '/' << pcb.totalTime;
        output << std::left
               << padRightDisplayWidth(std::to_string(pcb.pid), 6)
               << padRightDisplayWidth(std::to_string(pcb.ppid), 6)
               << padRightDisplayWidth(pcb.name, 14)
               << padRightDisplayWidth(toString(pcb.state), 18)
               << padRightDisplayWidth(std::to_string(pcb.priority), 6)
               << padRightDisplayWidth(queueName(pcb.queueLevel), 7)
               << padRightDisplayWidth(cpu.str(), 10)
               << padRightDisplayWidth(pcb.swappedOut ? "-" : std::to_string(pcb.memStart), 10)
               << padRightDisplayWidth(std::to_string(pcb.memSize), 8)
               << padRightDisplayWidth(pcb.swappedOut ? "是" : "否", 6)
               << '\n';
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
    output << "进程树 / Process Tree [用户: " << owner << "]";
    if (roots.empty()) {
        output << "\n当前用户没有进程。";
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
    output << "就绪队列:";
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
            output << "空";
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
        message = "[提示] 就绪队列校验通过。";
    } else {
        message = "[提示] 就绪队列校验修复了 " + std::to_string(removed) + " 个无效条目。";
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
    const auto queueIndex = static_cast<std::size_t>(it->second.queueLevel);
    if (queueIndex < readyQueues_.size()) {
        readyQueues_[queueIndex].push_back(pid);
    }
}

// collectSubtreeLocked：递归收集进程子树中的所有 PID（按 PID 排序）。
// 用于 kill_pcb 时收集需要删除的全部子孙进程。
// visited 集合防止环形父子关系导致无限递归。
// 收集顺序：父进程 → 子进程（PID 升序），保证 kill 输出顺序稳定。
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
        output << prefix << (isLast ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");
    }

    if (visited.find(pid) != visited.end()) {
        output << "PID=" << pid << " (cycle)";
        return;
    }

    visited.insert(pid);
    const auto& pcb = it->second;

    output << std::left
           << padRightDisplayWidth(pcb.name + "(" + std::to_string(pcb.pid) + ")", 24)
           << ' ' << padRightDisplayWidth(toString(pcb.state), 14)
           << " Prio=" << std::right << std::setw(2) << pcb.priority
           << "  Q" << pcb.queueLevel
           << "  CPU=" << std::setw(3) << pcb.executedTime << '/' << std::setw(3) << pcb.totalTime;
    if (pcb.swappedOut) {
        output << "  SWAPPED";
    } else {
        output << "  Mem=" << std::setw(3) << pcb.memStart << '+' << std::setw(3) << pcb.memSize << "KB";
    }

    std::vector<std::uint32_t> visibleChildren;
    for (const auto childPid : pcb.children) {
        auto child = pcbTable_.find(childPid);
        if (child != pcbTable_.end() && child->second.owner == owner) {
            visibleChildren.push_back(childPid);
        }
    }
    std::sort(visibleChildren.begin(), visibleChildren.end());

    const std::string childPrefix = isRoot ? "" : prefix + (isLast ? "   " : "\xe2\x94\x82  ");
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
