#pragma once

#include "process/PCB.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oscore {

// ProcessManager 维护 PCB 表和三层 MLFQ 就绪队列。
// 所有进程状态转换、父子关系维护、队列入队/出队都集中在这里完成，避免外部留下悬挂引用。
class ProcessManager {
public:
    bool createProcess(
        const std::string& owner,
        const std::string& name,
        std::uint32_t memKB,
        int priority,
        std::uint32_t totalTime,
        std::optional<std::uint32_t> ppid,
        std::string& message);
    bool createProcessWithMemory(
        const std::string& owner,
        const std::string& name,
        std::uint32_t memKB,
        std::uint32_t memStart,
        int priority,
        std::uint32_t totalTime,
        std::optional<std::uint32_t> ppid,
        std::uint32_t& outPid,
        std::string& message);

    bool killProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    bool killProcess(const std::string& owner, std::uint32_t pid, std::vector<std::uint32_t>& removedPids, std::string& message);
    bool blockProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    bool wakeupProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    bool suspendProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    bool resumeProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    bool reniceProcess(const std::string& owner, std::uint32_t pid, int newPriority, std::string& message);
    bool markSwappedOut(const std::string& owner, std::uint32_t pid, std::string& message);
    bool updateProcessMemoryStart(std::uint32_t pid, std::uint32_t newStart);
    [[nodiscard]] std::optional<std::uint32_t> pickNextReadyProcess(const std::string& owner);
    [[nodiscard]] bool removeFromReadyQueues(std::uint32_t pid);
    [[nodiscard]] bool enqueueReadyProcess(std::uint32_t pid);
    [[nodiscard]] bool demoteProcess(std::uint32_t pid);
    [[nodiscard]] bool markRunning(std::uint32_t pid);
    [[nodiscard]] bool markReady(std::uint32_t pid);
    [[nodiscard]] bool markTerminated(std::uint32_t pid);
    [[nodiscard]] bool tickProcess(std::uint32_t pid, std::uint32_t ticks, std::string& log);
    [[nodiscard]] std::optional<PCB> getProcessCopy(std::uint32_t pid) const;
    [[nodiscard]] std::vector<std::uint32_t> cleanupInvalidReadyQueueEntries(const std::string& owner);
    [[nodiscard]] bool hasProcess(const std::string& owner, std::uint32_t pid) const;
    [[nodiscard]] bool isSwappedOut(const std::string& owner, std::uint32_t pid) const;
    [[nodiscard]] std::vector<PCB> getProcessCopiesForUser(const std::string& owner) const;
    [[nodiscard]] std::uint32_t nextPid() const;
    [[nodiscard]] static std::uint32_t timeSliceForQueueLevel(int queueLevel);
    [[nodiscard]] static std::string queueNameForLevel(int queueLevel);

    [[nodiscard]] std::string showProcess(const std::string& owner, std::uint32_t pid) const;
    [[nodiscard]] std::string listProcesses(const std::string& owner) const;
    [[nodiscard]] std::string processTree(const std::string& owner) const;
    [[nodiscard]] std::string readyQueueSnapshot(const std::string& owner) const;
    [[nodiscard]] std::size_t processCount(const std::string& owner) const;

    [[nodiscard]] std::uint32_t exportNextPid() const;
    void importNextPid(std::uint32_t nextPid);
    [[nodiscard]] std::vector<PCB> exportPcbs() const;
    void importPcbs(const std::vector<PCB>& pcbs);
    [[nodiscard]] std::array<std::vector<std::uint32_t>, 3> exportReadyQueues() const;
    void importReadyQueues(const std::array<std::vector<std::uint32_t>, 3>& queues);
    void rebuildParentChildRelationsIfNeeded();
    bool validateReadyQueues(std::string& message);

private:
    [[nodiscard]] static bool isValidPriority(int priority);
    [[nodiscard]] static int queueLevelForPriority(int priority);
    [[nodiscard]] static std::uint32_t timeSliceForQueue(int queueLevel);
    [[nodiscard]] static std::string queueName(int queueLevel);

    [[nodiscard]] bool hasOwnedProcessLocked(const std::string& owner, std::uint32_t pid) const;
    void removeFromReadyQueuesLocked(std::uint32_t pid);
    void enqueueReadyLocked(std::uint32_t pid);
    void collectSubtreeLocked(
        const std::string& owner,
        std::uint32_t pid,
        std::vector<std::uint32_t>& ordered,
        std::unordered_set<std::uint32_t>& visited) const;
    void appendTreeNodeLocked(
        const std::string& owner,
        std::uint32_t pid,
        const std::string& prefix,
        bool isLast,
        bool isRoot,
        std::unordered_set<std::uint32_t>& visited,
        std::ostringstream& output) const;

    mutable std::mutex mutex_;
    std::uint32_t nextPid_ = 1;
    std::unordered_map<std::uint32_t, PCB> pcbTable_;
    std::array<std::deque<std::uint32_t>, 3> readyQueues_;
};

} // namespace oscore
