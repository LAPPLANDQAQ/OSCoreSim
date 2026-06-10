#include "process/ProcessManager.h"

#include "util/StringUtil.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace oscore {

namespace {

[[nodiscard]] std::vector<std::uint32_t> sortedIds(const std::vector<std::uint32_t>& ids) {
    // 复制一份 PID 列表再排序，避免修改 PCB.children 的原始顺序。
    auto result = ids;
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] std::string joinIds(const std::vector<std::uint32_t>& ids) {
    // 空列表用“无”显示，适合子进程列表和删除结果。
    if (ids.empty()) {
        return "无";
    }

    std::ostringstream output;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            // PID 之间用空格分隔，便于终端阅读。
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
    // 旧接口没有外部传入 memStart，统一转发到 createProcessWithMemory。
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
    // owner 为空说明没有登录用户，不能创建用户隔离的进程。
    if (owner.empty()) {
        message = "[失败] 创建失败：请先登录。";
        return false;
    }
    // 进程名不能为空，否则列表和进程树无法给出可读标识。
    if (name.empty()) {
        message = "[失败] 创建失败：进程名不能为空。";
        return false;
    }
    // 进程必须申请正数内存；内存是否足够由 MemoryManager 在调用前检查。
    if (memKB == 0) {
        message = "[失败] 创建失败：内存大小必须大于 0 KB。";
        return false;
    }
    // 优先级必须在 0-15，后续会映射到 Q0/Q1/Q2。
    if (!isValidPriority(priority)) {
        message = "[失败] 创建失败：优先级范围为 0-15。";
        return false;
    }
    // totalTime 为 0 的进程没有可调度意义，因此拒绝创建。
    if (totalTime == 0) {
        message = "[失败] 创建失败：总时间必须大于 0。";
        return false;
    }

    // 下面开始读写 pcbTable_、nextPid_ 和 readyQueues_，必须持锁。
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t parentPid = ppid.value_or(0);
    // 如果指定父 PID，父进程必须存在且属于同一用户。
    if (parentPid != 0 && !hasOwnedProcessLocked(owner, parentPid)) {
        message = "[失败] 创建失败：父 PID 不存在或访问被拒绝。";
        return false;
    }

    PCB pcb;
    // nextPid_ 全局单调递增，不因进程删除而回收复用。
    pcb.pid = nextPid_++;
    pcb.ppid = parentPid;
    pcb.name = name;
    pcb.owner = owner;
    pcb.state = ProcessState::READY;
    pcb.priority = priority;
    // 优先级映射到 MLFQ 层级：0-3→Q0，4-7→Q1，8-15→Q2。
    pcb.queueLevel = queueLevelForPriority(priority);
    pcb.totalTime = totalTime;
    pcb.executedTime = 0;
    pcb.remainingTime = totalTime;
    // 初始时间片由所在队列决定。
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);
    // memStart/memSize 来自 Kernel 与 MemoryManager 的分配结果。
    pcb.memStart = memStart;
    pcb.memSize = memKB;
    pcb.swappedOut = false;

    const std::uint32_t pid = pcb.pid;
    // 写入进程表后，后续所有操作都通过 PID 查找该 PCB。
    pcbTable_.emplace(pid, std::move(pcb));
    if (parentPid != 0) {
        // 父进程 children 记录子 PID，用于 ptree 展示和 kill_pcb 递归删除。
        pcbTable_.at(parentPid).children.push_back(pid);
    }
    // 新进程创建后直接进入 READY，并加入对应 MLFQ 队列。
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
    // 简化重载：调用方不关心释放内存时，只忽略 removedPids。
    std::vector<std::uint32_t> removed;
    return killProcess(owner, pid, removed, message);
}

bool ProcessManager::killProcess(
    const std::string& owner,
    std::uint32_t pid,
    std::vector<std::uint32_t>& removedPids,
    std::string& message) {
    // 删除会同时修改 PCB 表、父子关系和就绪队列，必须持锁。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasOwnedProcessLocked(owner, pid)) {
        message = "[失败] 删除失败：PID 不存在或访问被拒绝。";
        return false;
    }

    std::vector<std::uint32_t> removed;
    std::unordered_set<std::uint32_t> visited;
    // 先收集目标进程和全部子孙 PID，避免边遍历边删除破坏 children。
    collectSubtreeLocked(owner, pid, removed, visited);

    for (const auto removedPid : removed) {
        // 从所有就绪队列删除，保证被杀进程不会继续被调度。
        removeFromReadyQueuesLocked(removedPid);
        auto it = pcbTable_.find(removedPid);
        if (it != pcbTable_.end()) {
            // 删除前先标记 TERMINATED，便于调试观察状态转换语义。
            it->second.state = ProcessState::TERMINATED;
        }
    }

    for (auto& [_, pcb] : pcbTable_) {
        // 清理其他进程 children 中指向被删除子树的 PID。
        pcb.children.erase(
            std::remove_if(pcb.children.begin(), pcb.children.end(), [&visited](std::uint32_t childPid) {
                return visited.find(childPid) != visited.end();
            }),
            pcb.children.end());
    }

    for (const auto removedPid : removed) {
        // 最后从 pcbTable_ 真正移除 PCB 记录。
        pcbTable_.erase(removedPid);
    }

    std::ostringstream output;
    output << "[成功] 删除进程子树。已移除 PID：" << joinIds(removed);
    message = output.str();
    removedPids = removed;
    return true;
}

bool ProcessManager::blockProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    // 阻塞会修改状态和 readyQueues_，必须持锁。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 阻塞失败：PID 不存在或访问被拒绝。";
        return false;
    }

    auto& pcb = it->second;
    if (pcb.state != ProcessState::READY && pcb.state != ProcessState::RUNNING) {
        // 只有可运行或正在运行的进程可以被阻塞。
        message = "[失败] 阻塞失败：只能阻塞 READY 或 RUNNING 状态的进程。";
        return false;
    }

    const auto oldState = toString(pcb.state);
    // READY 进程可能在队列中，RUNNING 进程一般不在队列中；统一删除更安全。
    removeFromReadyQueuesLocked(pid);
    // 阻塞后不再参与调度。
    pcb.state = ProcessState::BLOCKED;

    std::ostringstream output;
    output << "[成功] PID=" << pid << " 已阻塞：" << oldState << " -> BLOCKED。";
    message = output.str();
    return true;
}

bool ProcessManager::wakeupProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    // 唤醒会把 BLOCKED 进程重新放入 readyQueues_。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 唤醒失败：PID 不存在或访问被拒绝。";
        return false;
    }

    auto& pcb = it->second;
    if (pcb.state != ProcessState::BLOCKED) {
        // 挂起阻塞进程需要先 resume，再 wakeup；这里仅处理普通 BLOCKED。
        message = "[失败] 唤醒失败：只能唤醒 BLOCKED 状态的进程。";
        return false;
    }

    // BLOCKED → READY，并按原 queueLevel 入队。
    pcb.state = ProcessState::READY;
    enqueueReadyLocked(pid);
    message = "[成功] PID=" + std::to_string(pid) + " 已唤醒：BLOCKED -> READY。";
    return true;
}

bool ProcessManager::suspendProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    // 挂起操作只改变调度可见性，不释放内存。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 挂起失败：PID 不存在或访问被拒绝。";
        return false;
    }

    auto& pcb = it->second;
    const auto oldState = toString(pcb.state);
    if (pcb.state == ProcessState::READY || pcb.state == ProcessState::RUNNING) {
        // 可运行类状态挂起后进入 SUSPENDED_READY，恢复后会回到 READY。
        removeFromReadyQueuesLocked(pid);
        pcb.state = ProcessState::SUSPENDED_READY;
    } else if (pcb.state == ProcessState::BLOCKED) {
        // 阻塞进程挂起后记为 SUSPENDED_BLOCKED，恢复后仍是 BLOCKED。
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
    // 恢复操作根据挂起前的类型回到 READY 或 BLOCKED。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 恢复失败：PID 不存在或访问被拒绝。";
        return false;
    }

    auto& pcb = it->second;
    const auto oldState = toString(pcb.state);
    if (pcb.state == ProcessState::SUSPENDED_READY) {
        // SUSPENDED_READY 恢复后可调度，因此重新入队。
        pcb.state = ProcessState::READY;
        enqueueReadyLocked(pid);
    } else if (pcb.state == ProcessState::SUSPENDED_BLOCKED) {
        // SUSPENDED_BLOCKED 恢复后仍等待事件，不进入就绪队列。
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
    // 新优先级必须先校验，避免进入临界区后才发现参数非法。
    if (!isValidPriority(newPriority)) {
        message = "[失败] 修改优先级失败：优先级范围为 0-15。";
        return false;
    }

    // 修改优先级会影响 queueLevel 和 readyQueues_。
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
        // READY 进程当前可能在旧队列中，先移除再按新队列重新入队。
        removeFromReadyQueuesLocked(pid);
    }

    // 更新优先级后重新计算 MLFQ 层级和时间片。
    pcb.priority = newPriority;
    pcb.queueLevel = queueLevelForPriority(newPriority);
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);

    if (pcb.state == ProcessState::READY) {
        // 只有 READY 进程需要立即进入新的队列。
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
    // 换出会改变 PCB 状态并从就绪队列移除。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        message = "[失败] 换出失败：PID 不存在或访问被拒绝。";
        return false;
    }
    auto& pcb = it->second;
    if (pcb.swappedOut || pcb.state == ProcessState::SWAPPED) {
        // 已换出的进程不允许重复换出。
        message = "[失败] 换出失败：该进程已被换出。";
        return false;
    }

    // 换出后不能继续被调度，因此先清理所有就绪队列中的 PID。
    removeFromReadyQueuesLocked(pid);
    // swappedOut 是快速标志，state=SWAPPED 是对外可见状态。
    pcb.swappedOut = true;
    // 换出后不再占用物理内存起始地址，但 memSize 仍保留需求大小。
    pcb.memStart = 0;
    pcb.state = ProcessState::SWAPPED;
    message = "[成功] PID=" + std::to_string(pid) + " 已标记为 SWAPPED。";
    return true;
}

std::optional<std::uint32_t> ProcessManager::pickNextReadyProcess(const std::string& owner) {
    // 调度器从这里取下一个 READY 进程，取出时会从队列移除。
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& queue : readyQueues_) {
        // readyQueues_ 的数组顺序就是 Q0 → Q1 → Q2 的优先级顺序。
        for (auto it = queue.begin(); it != queue.end();) {
            const auto pid = *it;
            auto pcbIt = pcbTable_.find(pid);
            if (pcbIt == pcbTable_.end() || pcbIt->second.state != ProcessState::READY || pcbIt->second.swappedOut) {
                // 发现陈旧或无效队列条目时顺手清理。
                it = queue.erase(it);
                continue;
            }
            if (pcbIt->second.owner != owner) {
                // 队列是全局结构，但调度器一次只服务一个 owner，其他用户 PID 暂时跳过。
                ++it;
                continue;
            }

            // 找到最高优先级可运行进程后从队列删除并返回 PID。
            queue.erase(it);
            return pid;
        }
    }

    return std::nullopt;
}

bool ProcessManager::removeFromReadyQueues(std::uint32_t pid) {
    // 公开接口加锁后调用同样的队列删除逻辑。
    std::lock_guard<std::mutex> lock(mutex_);
    bool removed = false;
    for (auto& queue : readyQueues_) {
        const auto oldSize = queue.size();
        // remove/erase 删除该 PID 的所有重复条目。
        queue.erase(std::remove(queue.begin(), queue.end(), pid), queue.end());
        removed = removed || oldSize != queue.size();
    }
    return removed;
}

bool ProcessManager::enqueueReadyProcess(std::uint32_t pid) {
    // 调度器把未完成进程放回 READY 队列时调用。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::READY || it->second.swappedOut) {
        // 只有存在、READY、未换出的进程可以入队。
        return false;
    }

    enqueueReadyLocked(pid);
    return true;
}

bool ProcessManager::demoteProcess(std::uint32_t pid) {
    // 进程用完整个时间片仍未完成时，调度器调用降级。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return false;
    }

    auto& pcb = it->second;
    if (pcb.queueLevel < 2) {
        // 最高只降到 Q2，Q2 继续保持最低优先级。
        ++pcb.queueLevel;
    }
    // 降级或保持 Q2 后都重置为当前队列默认时间片。
    pcb.timeSliceLeft = timeSliceForQueue(pcb.queueLevel);
    return true;
}

// markRunning：READY → RUNNING。
// 从就绪队列移除 → 状态改为 RUNNING → 确保 timeSliceLeft 非零（必要时重置为队列默认值）。
// Scheduler::step 在选中进程后调用此方法。
bool ProcessManager::markRunning(std::uint32_t pid) {
    // READY → RUNNING 是调度器选中进程后的状态转换。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::READY || it->second.swappedOut) {
        return false;
    }

    // RUNNING 进程不应继续留在 readyQueues_ 中。
    removeFromReadyQueuesLocked(pid);
    it->second.state = ProcessState::RUNNING;
    if (it->second.timeSliceLeft == 0) {
        // 如果导入快照或前序操作导致时间片为 0，则按队列层级补齐。
        it->second.timeSliceLeft = timeSliceForQueue(it->second.queueLevel);
    }
    return true;
}

bool ProcessManager::markReady(std::uint32_t pid) {
    // RUNNING 进程在一个 step 后若未完成，会先标记回 READY。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() ||
        it->second.state == ProcessState::TERMINATED ||
        it->second.state == ProcessState::SWAPPED ||
        it->second.swappedOut) {
        // 已终止或换出的进程不能重新进入 READY。
        return false;
    }

    it->second.state = ProcessState::READY;
    if (it->second.timeSliceLeft == 0) {
        // 没有剩余时间片时按当前队列重置。
        it->second.timeSliceLeft = timeSliceForQueue(it->second.queueLevel);
    }
    return true;
}

bool ProcessManager::markTerminated(std::uint32_t pid) {
    // Scheduler 发现进程执行完成后调用此函数。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return false;
    }

    // 终止进程不应再被调度。
    removeFromReadyQueuesLocked(pid);
    it->second.state = ProcessState::TERMINATED;
    return true;
}

bool ProcessManager::tickProcess(std::uint32_t pid, std::uint32_t ticks, std::string& log) {
    // tickProcess 模拟 CPU 时间推进，会修改 executedTime、remainingTime 和 timeSliceLeft。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::RUNNING || ticks == 0) {
        log = "[失败] Tick 失败：PID 不存在或不是 RUNNING 状态。";
        return false;
    }

    auto& pcb = it->second;
    std::ostringstream output;
    for (std::uint32_t tick = 1; tick <= ticks && pcb.remainingTime > 0; ++tick) {
        // 每个 tick 增加已执行时间。
        ++pcb.executedTime;
        // 同时减少剩余总运行时间。
        --pcb.remainingTime;
        if (pcb.timeSliceLeft > 0) {
            // 时间片只在大于 0 时递减，避免 uint32 下溢。
            --pcb.timeSliceLeft;
        }
        output << "tick " << tick << '/' << ticks
               << ": PID=" << pid
               << " 执行=" << pcb.executedTime << '/' << pcb.totalTime
               << ", 剩余=" << pcb.remainingTime << '\n';
    }

    log = output.str();
    if (!log.empty() && log.back() == '\n') {
        // 去掉末尾换行，便于 Scheduler 拼接日志。
        log.pop_back();
    }
    return true;
}

std::optional<PCB> ProcessManager::getProcessCopy(std::uint32_t pid) const {
    // 返回 PCB 副本，避免调用方拿到内部引用后绕过锁修改。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::uint32_t> ProcessManager::cleanupInvalidReadyQueueEntries(const std::string& owner) {
    // owner 当前未参与过滤；保留参数是为了和调度器重启语义保持一致。
    (void)owner;
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> removed;
    for (auto& queue : readyQueues_) {
        for (auto it = queue.begin(); it != queue.end();) {
            auto pcbIt = pcbTable_.find(*it);
            if (pcbIt == pcbTable_.end() || pcbIt->second.state != ProcessState::READY || pcbIt->second.swappedOut) {
                // 队列中不存在、非 READY 或已换出的 PID 都是无效条目。
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
    // compact 后 Kernel 使用此函数把 MemoryManager 新地址同步回 PCB。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end()) {
        return false;
    }
    // 只改 memStart，不改变 memSize、状态或队列。
    it->second.memStart = newStart;
    return true;
}

bool ProcessManager::hasProcess(const std::string& owner, std::uint32_t pid) const {
    // 公共查询加锁后复用私有权限检查。
    std::lock_guard<std::mutex> lock(mutex_);
    return hasOwnedProcessLocked(owner, pid);
}

bool ProcessManager::isSwappedOut(const std::string& owner, std::uint32_t pid) const {
    // 查询某用户的某进程是否已经换出。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    return it != pcbTable_.end() && it->second.owner == owner && it->second.swappedOut;
}

std::vector<PCB> ProcessManager::getProcessCopiesForUser(const std::string& owner) const {
    // overview 渲染需要当前用户的 PCB 副本集合。
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PCB> copies;
    copies.reserve(pcbTable_.size());
    for (const auto& [_, pcb] : pcbTable_) {
        if (pcb.owner == owner) {
            // 只导出当前用户进程，保证多用户隔离。
            copies.push_back(pcb);
        }
    }
    // 按 PID 排序，保证显示顺序稳定。
    std::sort(copies.begin(), copies.end(), [](const PCB& left, const PCB& right) {
        return left.pid < right.pid;
    });
    return copies;
}

std::uint32_t ProcessManager::nextPid() const {
    // Kernel 创建进程前读取 nextPid_，用于提前给 MemoryManager 绑定 PID。
    std::lock_guard<std::mutex> lock(mutex_);
    return nextPid_;
}

std::uint32_t ProcessManager::timeSliceForQueueLevel(int queueLevel) {
    // 对外暴露队列层级到时间片的映射，Scheduler 打印日志时可使用。
    return timeSliceForQueue(queueLevel);
}

std::string ProcessManager::queueNameForLevel(int queueLevel) {
    // 对外暴露 Q0/Q1/Q2 名称生成逻辑，避免重复写字符串。
    return queueName(queueLevel);
}

std::string ProcessManager::showProcess(const std::string& owner, std::uint32_t pid) const {
    // show_pcb 查询单个 PCB 的完整字段。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        return "[失败] 进程不存在或访问被拒绝。";
    }

    const auto& pcb = it->second;
    std::ostringstream output;
    // 逐字段输出 PCB 内容，帮助课程报告解释 PCB 结构。
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
        // 换出进程没有有效物理起始地址，只展示需求大小。
        output << "已换出，需求=" << pcb.memSize << "KB\n";
    } else {
        // 未换出时展示物理内存起点和大小。
        output << "起始=" << pcb.memStart << "KB, 大小=" << pcb.memSize << "KB\n";
    }
    output
           << "是否换出: " << (pcb.swappedOut ? "是" : "否") << '\n'
           << "子进程: " << joinIds(sortedIds(pcb.children));
    return output.str();
}

std::string ProcessManager::listProcesses(const std::string& owner) const {
    // list_pcb 输出当前用户所有进程的表格。
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PCB> visible;
    for (const auto& [_, pcb] : pcbTable_) {
        if (pcb.owner == owner) {
            // 过滤掉其他用户进程。
            visible.push_back(pcb);
        }
    }
    // 按 PID 排序，让表格输出稳定。
    std::sort(visible.begin(), visible.end(), [](const PCB& left, const PCB& right) {
        return left.pid < right.pid;
    });

    if (visible.empty()) {
        return "当前用户没有进程。";
    }

    std::ostringstream output;
    // 表头使用 padRightDisplayWidth，兼容中文和英文混合宽度。
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
        // CPU 列展示已执行/总时间。
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
        // 去掉最后一行多余换行，避免命令输出末尾多空行。
        result.pop_back();
    }
    return result;
}

std::string ProcessManager::processTree(const std::string& owner) const {
    // ptree 从根进程开始递归渲染当前用户进程树。
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> roots;
    for (const auto& [pid, pcb] : pcbTable_) {
        if (pcb.owner != owner) {
            continue;
        }
        const auto parent = pcbTable_.find(pcb.ppid);
        if (pcb.ppid == 0 || parent == pcbTable_.end() || parent->second.owner != owner) {
            // 无父进程、父进程不存在或父进程不属于当前用户时，都作为根节点显示。
            roots.push_back(pid);
        }
    }
    // 根节点按 PID 排序，保证树输出稳定。
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
        // 每个根节点递归输出其子树。
        appendTreeNodeLocked(owner, roots[i], "", i + 1 == roots.size(), true, visited, output);
    }
    return output.str();
}

std::string ProcessManager::readyQueueSnapshot(const std::string& owner) const {
    // readyq 展示当前用户在 Q0/Q1/Q2 中的 READY 进程。
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "就绪队列:";
    for (std::size_t q = 0; q < readyQueues_.size(); ++q) {
        output << '\n' << queueName(static_cast<int>(q)) << ": ";
        bool first = true;
        for (const auto pid : readyQueues_[q]) {
            auto it = pcbTable_.find(pid);
            if (it == pcbTable_.end() || it->second.owner != owner || it->second.state != ProcessState::READY) {
                // 输出时跳过不存在、非当前用户或非 READY 的队列条目。
                continue;
            }
            if (!first) {
                output << ' ';
            }
            output << pid;
            first = false;
        }
        if (first) {
            // 当前队列没有可显示 PID 时输出“空”。
            output << "空";
        }
    }
    return output.str();
}

std::size_t ProcessManager::processCount(const std::string& owner) const {
    // status/overview 使用的当前用户进程数量统计。
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(pcbTable_.begin(), pcbTable_.end(), [&owner](const auto& item) {
        return item.second.owner == owner;
    }));
}

std::uint32_t ProcessManager::exportNextPid() const {
    // 持久化 nextPid_，保证加载后不会复用已有 PID。
    std::lock_guard<std::mutex> lock(mutex_);
    return nextPid_;
}

void ProcessManager::importNextPid(std::uint32_t nextPid) {
    // 导入 nextPid_ 时至少为 1，避免生成 PID=0。
    std::lock_guard<std::mutex> lock(mutex_);
    nextPid_ = std::max<std::uint32_t>(nextPid, 1);
}

std::vector<PCB> ProcessManager::exportPcbs() const {
    // 导出 PCB 表供 SnapshotStore 按字段序列化。
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PCB> pcbs;
    pcbs.reserve(pcbTable_.size());
    for (const auto& [_, pcb] : pcbTable_) {
        // 复制完整 PCB，包括父子关系、时间片、内存和换出状态。
        pcbs.push_back(pcb);
    }
    // 二进制快照按 PID 顺序写入，便于调试和兼容。
    std::sort(pcbs.begin(), pcbs.end(), [](const PCB& left, const PCB& right) {
        return left.pid < right.pid;
    });
    return pcbs;
}

void ProcessManager::importPcbs(const std::vector<PCB>& pcbs) {
    // 导入 PCB 会重建进程表和 READY 队列。
    std::lock_guard<std::mutex> lock(mutex_);
    pcbTable_.clear();
    for (auto& queue : readyQueues_) {
        // 旧就绪队列必须清空，避免快照外的 PID 残留。
        queue.clear();
    }

    // 先导入 PCB，并根据最大 PID 推导 nextPid_。
    nextPid_ = 1;
    for (const auto& pcb : pcbs) {
        pcbTable_[pcb.pid] = pcb;
        nextPid_ = std::max(nextPid_, pcb.pid + 1);
    }

    // 再把 READY 进程按自身 queueLevel 放入对应队列。
    for (const auto& [pid, pcb] : pcbTable_) {
        if (pcb.state == ProcessState::READY) {
            readyQueues_[static_cast<std::size_t>(pcb.queueLevel)].push_back(pid);
        }
    }
}

std::array<std::vector<std::uint32_t>, 3> ProcessManager::exportReadyQueues() const {
    // 就绪队列保存 PID 顺序，恢复后调度顺序才能一致。
    std::lock_guard<std::mutex> lock(mutex_);
    std::array<std::vector<std::uint32_t>, 3> queues;
    for (std::size_t i = 0; i < readyQueues_.size(); ++i) {
        // deque 转 vector，便于 SnapshotStore 序列化。
        queues[i].assign(readyQueues_[i].begin(), readyQueues_[i].end());
    }
    return queues;
}

void ProcessManager::importReadyQueues(const std::array<std::vector<std::uint32_t>, 3>& queues) {
    // 导入外部保存的队列顺序，后续 validateReadyQueues 会清理无效条目。
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < readyQueues_.size(); ++i) {
        readyQueues_[i].clear();
        readyQueues_[i].insert(readyQueues_[i].end(), queues[i].begin(), queues[i].end());
    }
}

void ProcessManager::rebuildParentChildRelationsIfNeeded() {
    // 快照可能只可靠保存 ppid，因此加载后可用 ppid 反推 children。
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, pcb] : pcbTable_) {
        // 先清空旧 children，避免重复累加。
        pcb.children.clear();
    }

    for (const auto& [pid, pcb] : pcbTable_) {
        if (pcb.ppid == 0) {
            continue;
        }
        auto parent = pcbTable_.find(pcb.ppid);
        if (parent != pcbTable_.end() && parent->second.owner == pcb.owner) {
            // 只有同一用户的父进程才接收该子 PID。
            parent->second.children.push_back(pid);
        }
    }

    for (auto& [_, pcb] : pcbTable_) {
        // 排序并去重，保证 ptree 和 kill 输出稳定。
        std::sort(pcb.children.begin(), pcb.children.end());
        pcb.children.erase(std::unique(pcb.children.begin(), pcb.children.end()), pcb.children.end());
    }
}

bool ProcessManager::validateReadyQueues(std::string& message) {
    // 加载快照后校验 readyQueues_，移除不存在或状态不匹配的 PID。
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
                // 无效条目直接删除，并累计修复数量。
                it = queue.erase(it);
                ++removed;
            } else {
                // seen 用于发现跨队列或同队列重复 PID。
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
    // 课程要求优先级范围固定为 0-15。
    return priority >= 0 && priority <= 15;
}

int ProcessManager::queueLevelForPriority(int priority) {
    // 0-3 是最高优先级队列 Q0。
    if (priority <= 3) {
        return 0;
    }
    // 4-7 是中间队列 Q1。
    if (priority <= 7) {
        return 1;
    }
    // 8-15 是最低队列 Q2。
    return 2;
}

std::uint32_t ProcessManager::timeSliceForQueue(int queueLevel) {
    // MLFQ 设计：高优先级短时间片，低优先级长时间片。
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
    // 队列层级 0/1/2 显示为 Q0/Q1/Q2。
    return "Q" + std::to_string(queueLevel);
}

bool ProcessManager::hasOwnedProcessLocked(const std::string& owner, std::uint32_t pid) const {
    // 私有 locked 版本不加锁，调用方必须已经持有 mutex_。
    auto it = pcbTable_.find(pid);
    return it != pcbTable_.end() && it->second.owner == owner;
}

void ProcessManager::removeFromReadyQueuesLocked(std::uint32_t pid) {
    // 从三个 MLFQ 队列中删除该 PID 的所有出现位置。
    for (auto& queue : readyQueues_) {
        queue.erase(std::remove(queue.begin(), queue.end(), pid), queue.end());
    }
}

void ProcessManager::enqueueReadyLocked(std::uint32_t pid) {
    // 入队前先确认 PCB 存在且状态确实为 READY。
    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.state != ProcessState::READY) {
        return;
    }

    // 先移除旧条目，防止同一个 PID 在队列中重复出现。
    removeFromReadyQueuesLocked(pid);
    const auto queueIndex = static_cast<std::size_t>(it->second.queueLevel);
    if (queueIndex < readyQueues_.size()) {
        // 按 queueLevel 插入 Q0/Q1/Q2 队尾。
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
        // 防止异常快照形成环时无限递归。
        return;
    }

    auto it = pcbTable_.find(pid);
    if (it == pcbTable_.end() || it->second.owner != owner) {
        // 不存在或不属于当前用户的节点不参与当前子树。
        return;
    }

    // 先记录父节点，再递归子节点。
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
        // 只渲染当前用户可见的节点。
        return;
    }

    if (!isRoot) {
        // 非根节点输出树枝前缀，使用 UTF-8 线条字符。
        output << prefix << (isLast ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");
    }

    if (visited.find(pid) != visited.end()) {
        // 检测到环时只输出提示，不继续递归。
        output << "PID=" << pid << " (cycle)";
        return;
    }

    // 标记已访问，防止异常 children 关系重复渲染。
    visited.insert(pid);
    const auto& pcb = it->second;

    // 当前节点输出名称、PID、状态、优先级、队列和 CPU 时间。
    output << std::left
           << padRightDisplayWidth(pcb.name + "(" + std::to_string(pcb.pid) + ")", 24)
           << ' ' << padRightDisplayWidth(toString(pcb.state), 14)
           << " Prio=" << std::right << std::setw(2) << pcb.priority
           << "  Q" << pcb.queueLevel
           << "  CPU=" << std::setw(3) << pcb.executedTime << '/' << std::setw(3) << pcb.totalTime;
    if (pcb.swappedOut) {
        // 换出进程用 SWAPPED 标记代替物理内存地址。
        output << "  SWAPPED";
    } else {
        // 未换出时显示起始地址 + 大小。
        output << "  Mem=" << std::setw(3) << pcb.memStart << '+' << std::setw(3) << pcb.memSize << "KB";
    }

    std::vector<std::uint32_t> visibleChildren;
    for (const auto childPid : pcb.children) {
        auto child = pcbTable_.find(childPid);
        if (child != pcbTable_.end() && child->second.owner == owner) {
            // 只把当前用户可见的子进程加入渲染列表。
            visibleChildren.push_back(childPid);
        }
    }
    // 子节点按 PID 排序，保证输出稳定。
    std::sort(visibleChildren.begin(), visibleChildren.end());

    // 子节点前缀根据当前节点是否为最后一个兄弟节点决定。
    const std::string childPrefix = isRoot ? "" : prefix + (isLast ? "   " : "\xe2\x94\x82  ");
    for (std::size_t i = 0; i < visibleChildren.size(); ++i) {
        output << '\n';
        // 递归渲染每个子节点。
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
