#pragma once

#include "memory/MemoryBlock.h"
#include "process/PCB.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oscore {

// OverviewRenderer —— 只读可视化渲染器
//
// 负责将 Kernel 锁保护期间获取的系统状态快照渲染为 overview 输出字符串。
// 本类不持有任何可变状态，不执行 I/O，不修改 PCB、内存或调度队列。
class OverviewRenderer {
public:
    struct SchedulerInfo {
        bool running = false;
        std::string owner;
        int intervalMs = 500;
    };

    // 渲染完整 overview 输出。
    // 所有参数均为调用方在持锁期间获取的快照/副本，OverviewRenderer 自身不加锁。
    [[nodiscard]] std::string render(
        const std::string& currentUser,
        const std::vector<PCB>& userProcesses,
        const std::array<std::vector<std::uint32_t>, 3>& readyQueues,
        const std::vector<MemoryBlock>& memoryBlocks,
        std::uint32_t totalMemoryKB,
        const SchedulerInfo& schedulerInfo,
        const std::string& snapshotPath,
        const std::string& algorithmName,
        std::size_t vfsFileCount) const;

private:
    // [1] 系统摘要
    [[nodiscard]] std::string renderSystemSummary(
        const std::string& currentUser,
        std::size_t processCount,
        std::uint32_t totalMemoryKB,
        std::uint32_t usedMemoryKB,
        std::uint32_t freeMemoryKB,
        std::uint32_t largestFreeKB,
        double fragmentationRate,
        const SchedulerInfo& schedulerInfo,
        const std::string& snapshotPath,
        const std::string& algorithmName,
        std::size_t vfsFileCount) const;

    // [2] 进程表
    [[nodiscard]] static std::string renderProcessTable(
        const std::vector<PCB>& userProcesses);

    // [3] 进程树
    [[nodiscard]] std::string renderProcessTree(
        const std::string& currentUser,
        const std::vector<PCB>& userProcesses) const;

    // [4] 内存分区图
    [[nodiscard]] std::string renderMemoryMap(
        const std::string& currentUser,
        const std::vector<MemoryBlock>& memoryBlocks,
        std::uint32_t totalMemoryKB) const;

    // [5] MLFQ 多级反馈队列
    [[nodiscard]] std::string renderMLFQ(
        const std::string& currentUser,
        const std::array<std::vector<std::uint32_t>, 3>& readyQueues,
        const std::vector<PCB>& userProcesses) const;

    // [6] Notes
    [[nodiscard]] static std::string renderNotes();

    // 辅助：构建进程树节点
    void appendTreeNode(
        std::uint32_t pid,
        const std::string& prefix,
        bool isLast,
        bool isRoot,
        const std::unordered_map<std::uint32_t, PCB>& pcbMap,
        std::unordered_set<std::uint32_t>& visited,
        std::ostringstream& output) const;

    // 辅助：获取进程节点显示字符串
    [[nodiscard]] static std::string nodeText(const PCB& pcb);

    // 辅助：队列名称
    [[nodiscard]] static std::string queueName(int level);
    [[nodiscard]] static int quantumForQueue(int level);
    [[nodiscard]] static std::string priorityRangeForQueue(int level);
};

} // namespace oscore
