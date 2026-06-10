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

// ProcessManager：PCB 表管理器 + 三级 MLFQ 就绪队列管理器。
//
// 职责：
// - 维护以 PID 为键的 PCB 哈希表（pcbTable_）
// - 维护 3 层就绪队列（readyQueues_：Q0/Q1/Q2），存放 PID
// - 所有进程状态转换（NEW→READY→RUNNING→BLOCKED→...）都在此完成
// - 父子进程关系维护（children 字段）、递归子树的收集与删除
// - 进程列表、进程树、就绪队列快照的格式化输出
//
// 线程安全：所有公开方法内部加锁（mutex_），Kernel 只需串行调用即可。
// Locked 后缀的私有方法假定调用者已持锁。
class ProcessManager {
public:
    // === 进程生命周期 ===

    // 创建进程（alloc-size 版本，内存由 Kernel 分配后传入 memStart）
    bool createProcess(const std::string& owner, const std::string& name,
        std::uint32_t memKB, int priority, std::uint32_t totalTime,
        std::optional<std::uint32_t> ppid, std::string& message);
    bool createProcessWithMemory(const std::string& owner, const std::string& name,
        std::uint32_t memKB, std::uint32_t memStart, int priority,
        std::uint32_t totalTime, std::optional<std::uint32_t> ppid,
        std::uint32_t& outPid, std::string& message);

    // 递归删除进程子树：收集所有子孙 PID → 移除就绪队列 → 标记 TERMINATED → 清理 children 引用 → 删除 PCB
    bool killProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    bool killProcess(const std::string& owner, std::uint32_t pid,
        std::vector<std::uint32_t>& removedPids, std::string& message);

    // 状态转换（8 状态状态机）
    // READY/RUNNING → BLOCKED：从就绪队列移除，设为 BLOCKED
    bool blockProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    // BLOCKED → READY：重新加入就绪队列
    bool wakeupProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    // READY/RUNNING → SUSPENDED_READY，BLOCKED → SUSPENDED_BLOCKED
    bool suspendProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    // SUSPENDED_READY → READY，SUSPENDED_BLOCKED → BLOCKED
    bool resumeProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    // 修改优先级并重新计算队列层级（READY 进程需先移除再按新层级入队）
    bool reniceProcess(const std::string& owner, std::uint32_t pid, int newPriority, std::string& message);
    // 标记为 SWAPPED：从就绪队列移除，保留所需内存大小，便于缺页恢复
    bool markSwappedOut(const std::string& owner, std::uint32_t pid, std::string& message);
    // 更新进程内存起始地址（compact 后 Kernel 回写 PCB::memStart）
    bool updateProcessMemoryStart(std::uint32_t pid, std::uint32_t newStart);

    // === 调度器接口 ===

    // 从 Q0→Q1→Q2 扫描，返回第一个属于 owner 的 READY 进程 PID（同时从队列中移除）
    [[nodiscard]] std::optional<std::uint32_t> pickNextReadyProcess(const std::string& owner);
    // 只读检查当前用户是否至少有一个可调度的 READY 进程。
    // 以 pcbTable_ 为权威数据源，不依赖 readyQueues_（可能含过期条目）。
    // 供自动调度器启动前和 step 后判断是否还有可调度对象。
    [[nodiscard]] bool hasReadyProcessForUser(const std::string& owner) const;
    // 从所有就绪队列中移除指定 PID（用于状态变更前清理）
    [[nodiscard]] bool removeFromReadyQueues(std::uint32_t pid);
    // 将 READY 进程按 queueLevel 重新入队
    [[nodiscard]] bool enqueueReadyProcess(std::uint32_t pid);
    // MLFQ 降级：queueLevel++（最大 Q2），重置时间片为新队列量程
    [[nodiscard]] bool demoteProcess(std::uint32_t pid);
    // READY → RUNNING（移除队列 + 改状态 + 确保时间片非零）
    [[nodiscard]] bool markRunning(std::uint32_t pid);
    // 回到 READY（状态改为 READY + 确保时间片非零）
    [[nodiscard]] bool markReady(std::uint32_t pid);
    // 标记为 TERMINATED（移除队列 + 改状态）
    [[nodiscard]] bool markTerminated(std::uint32_t pid);
    // 逐 tick 推进进程执行，更新 executedTime / remainingTime / timeSliceLeft
    [[nodiscard]] bool tickProcess(std::uint32_t pid, std::uint32_t ticks, std::string& log);

    // === 查询接口 ===

    [[nodiscard]] std::optional<PCB> getProcessCopy(std::uint32_t pid) const;
    [[nodiscard]] std::vector<std::uint32_t> cleanupInvalidReadyQueueEntries(const std::string& owner);
    [[nodiscard]] bool hasProcess(const std::string& owner, std::uint32_t pid) const;
    [[nodiscard]] bool isSwappedOut(const std::string& owner, std::uint32_t pid) const;
    [[nodiscard]] std::vector<PCB> getProcessCopiesForUser(const std::string& owner) const;
    [[nodiscard]] std::uint32_t nextPid() const;
    [[nodiscard]] static std::uint32_t timeSliceForQueueLevel(int queueLevel);
    [[nodiscard]] static std::string queueNameForLevel(int queueLevel);

    // === 显示接口（格式化输出） ===

    [[nodiscard]] std::string showProcess(const std::string& owner, std::uint32_t pid) const;
    [[nodiscard]] std::string listProcesses(const std::string& owner) const;
    [[nodiscard]] std::string processTree(const std::string& owner) const;
    [[nodiscard]] std::string readyQueueSnapshot(const std::string& owner) const;
    [[nodiscard]] std::size_t processCount(const std::string& owner) const;

    // === 持久化接口 ===

    [[nodiscard]] std::uint32_t exportNextPid() const;
    void importNextPid(std::uint32_t nextPid);
    [[nodiscard]] std::vector<PCB> exportPcbs() const;
    void importPcbs(const std::vector<PCB>& pcbs);
    [[nodiscard]] std::array<std::vector<std::uint32_t>, 3> exportReadyQueues() const;
    void importReadyQueues(const std::array<std::vector<std::uint32_t>, 3>& queues);
    // 载入快照后根据 ppid 重建 children 列表
    void rebuildParentChildRelationsIfNeeded();
    // 验证就绪队列一致性：检查状态、队列层级、重复 PID
    bool validateReadyQueues(std::string& message);

private:
    // === 工具函数 ===

    [[nodiscard]] static bool isValidPriority(int priority);
    // 优先级 → 队列层级映射：0-3→Q0, 4-7→Q1, 8-15→Q2
    [[nodiscard]] static int queueLevelForPriority(int priority);
    // 每级队列的时间片：Q0=2, Q1=4, Q2=8
    [[nodiscard]] static std::uint32_t timeSliceForQueue(int queueLevel);
    [[nodiscard]] static std::string queueName(int queueLevel);

    // === 内部辅助（调用者需持有 mutex_） ===

    [[nodiscard]] bool hasOwnedProcessLocked(const std::string& owner, std::uint32_t pid) const;
    void removeFromReadyQueuesLocked(std::uint32_t pid);
    void enqueueReadyLocked(std::uint32_t pid);
    // 递归收集进程子树中的所有 PID（按 PID 排序），用于 kill
    void collectSubtreeLocked(const std::string& owner, std::uint32_t pid,
        std::vector<std::uint32_t>& ordered, std::unordered_set<std::uint32_t>& visited) const;
    // 递归渲染进程树节点（用于 processTree 输出）
    void appendTreeNodeLocked(const std::string& owner, std::uint32_t pid,
        const std::string& prefix, bool isLast, bool isRoot,
        std::unordered_set<std::uint32_t>& visited, std::ostringstream& output) const;

    mutable std::mutex mutex_;                          // 保护所有数据成员
    std::uint32_t nextPid_ = 1;                        // 下一个可分配的 PID，全局递增不重用
    std::unordered_map<std::uint32_t, PCB> pcbTable_;  // PID → PCB 映射表
    std::array<std::deque<std::uint32_t>, 3> readyQueues_;  // Q0/Q1/Q2 就绪队列，每队列存放 PID
};

} // namespace oscore
