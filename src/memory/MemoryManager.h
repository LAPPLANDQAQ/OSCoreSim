#pragma once

#include "memory/MemoryBlock.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace oscore {

// MemoryManager：动态分区内存管理器。
//
// 核心数据结构：
//   blocks_：按 start 升序排列的 MemoryBlock 向量，覆盖 [0, totalMemoryKB_) 的全部物理内存。
//   初始状态为单个 FREE 块。
//
// 分配算法（通过 findFreeBlockLocked 实现）：
//   - FF（首次适应）：选择第一个足够大的空闲块，O(n) 但通常最快
//   - BF（最佳适应）：选择最小的足够大的空闲块，减少内部浪费
//   - WF（最差适应）：选择最大的空闲块，试图保留大块给后续大请求
//
// 释放与合并：
//   释放后自动调用 mergeFreeBlocksLocked()，合并相邻 FREE 块，降低外部碎片。
//
// 紧缩（compact）：
//   将所有已分配块移到低地址端，消除空闲块间隙，同步更新 PROCESS 块的 PCB::memStart。
//
// 线程安全：所有公开方法内部加锁（mutex_）。
class MemoryManager {
public:
    MemoryManager();  // 初始化：创建单个覆盖全部内存的 FREE 块

    // === 分配接口 ===

    // 手动分配（alloc 命令）：生成 KERNEL 类型块，tag 默认为 "manual"
    bool allocateManual(const std::string& owner, std::uint32_t sizeKB,
        std::uint32_t& outStart, std::string& message);
    bool allocateManual(const std::string& owner, const std::string& tag,
        std::uint32_t sizeKB, std::uint32_t& outStart, std::string& message);
    // 进程内存分配（create_pcb 内部调用）：生成 PROCESS 类型块，与 PCB 绑定
    bool allocateForProcess(const std::string& owner, std::uint32_t pid,
        const std::string& processName, std::uint32_t sizeKB,
        std::uint32_t& outStart, std::string& message);

    // === 释放接口 ===

    // 按起始地址释放（free_mem 命令）：仅释放当前用户的 KERNEL 手动块
    bool freeByAddress(const std::string& owner, std::uint32_t addr, std::string& message);
    // 按 PID 释放（kill_pcb / 调度完成时调用）：释放 PROCESS 块
    bool freeByPid(const std::string& owner, std::uint32_t pid, std::string& message);
    // 换出进程（swap_out 命令）：释放 PROCESS 块的物理内存
    bool swapOutProcess(const std::string& owner, std::uint32_t pid, std::string& message);

    // === 紧缩 ===

    // 将所有已分配块移到低地址端，消除外部碎片
    // 返回 CompactionResult，其中 pidNewStart 记录了 PROCESS 块的新地址
    [[nodiscard]] CompactionResult compact();

    // === 显示接口 ===

    [[nodiscard]] std::string showMemory(const std::string& owner) const;  // 内存分区表 + ASCII 内存图
    [[nodiscard]] std::string memoryStat() const;                          // 内存统计摘要

    // === 算法切换 ===

    bool setAlgorithm(const std::string& algoName, std::string& message);
    [[nodiscard]] AllocAlgorithm currentAlgorithm() const;
    [[nodiscard]] std::string currentAlgorithmName() const;

    // === 查询接口 ===

    [[nodiscard]] std::uint32_t totalMemoryKB() const;
    void setTotalMemoryKB(std::uint32_t totalMemoryKB);
    [[nodiscard]] std::uint32_t usedMemoryKB() const;
    [[nodiscard]] std::uint32_t freeMemoryKB() const;
    void setAlgorithmDirect(AllocAlgorithm algorithm);  // 供快照导入直接设置
    bool validateBlocks(std::string& message) const;     // 验证内存块表的连续性、覆盖性和一致性

    // === 持久化接口 ===

    [[nodiscard]] std::vector<MemoryBlock> exportBlocks() const;
    void importBlocks(const std::vector<MemoryBlock>& blocks);  // 导入后自动排序并合并 FREE 块

private:
    // 通用分配逻辑（三种分配接口的底层实现）
    bool allocateLocked(const std::string& owner, std::uint32_t pid,
        const std::string& tag, std::uint32_t sizeKB, MemoryBlockType type,
        std::uint32_t& outStart, std::string& message);

    // 根据当前算法从空闲块中查找合适的候选块
    // FF：扫描到第一个足够大的空闲块立即返回
    // BF：遍历全部空闲块，选择最小的可用块
    // WF：遍历全部空闲块，选择最大的可用块
    [[nodiscard]] std::vector<MemoryBlock>::iterator findFreeBlockLocked(std::uint32_t sizeKB);

    // 按 start 升序排序（分配/释放/导入后调用，保证块连续有序）
    void sortBlocksLocked();
    // 合并相邻 FREE 块（释放后调用，降低外部碎片）
    void mergeFreeBlocksLocked();

    [[nodiscard]] static std::string normalizeAlgorithmName(std::string value);

    mutable std::mutex mutex_;
    std::uint32_t totalMemoryKB_ = 1024;                   // 物理内存总量（KB）
    AllocAlgorithm algorithm_ = AllocAlgorithm::FIRST_FIT;  // 当前分配算法
    std::vector<MemoryBlock> blocks_;                       // 内存块表，按 start 升序
};

} // namespace oscore
