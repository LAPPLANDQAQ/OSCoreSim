#pragma once

#include "memory/MemoryBlock.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace oscore {

// MemoryManager 实现首次适应/最佳适应/最坏适应动态分区分配。
// 它只管理内存块表；进程创建、终止和 compact 后的 PCB 同步由 Kernel/Dispatcher 协调。
class MemoryManager {
public:
    MemoryManager();

    bool allocateManual(const std::string& owner, std::uint32_t sizeKB, std::uint32_t& outStart, std::string& message);
    bool allocateForProcess(
        const std::string& owner,
        std::uint32_t pid,
        const std::string& processName,
        std::uint32_t sizeKB,
        std::uint32_t& outStart,
        std::string& message);

    bool freeByAddress(const std::string& owner, std::uint32_t addr, std::string& message);
    bool freeByPid(const std::string& owner, std::uint32_t pid, std::string& message);
    bool swapOutProcess(const std::string& owner, std::uint32_t pid, std::string& message);
    [[nodiscard]] CompactionResult compact();

    [[nodiscard]] std::string showMemory(const std::string& owner) const;
    [[nodiscard]] std::string memoryStat() const;
    bool setAlgorithm(const std::string& algoName, std::string& message);
    [[nodiscard]] AllocAlgorithm currentAlgorithm() const;
    [[nodiscard]] std::string currentAlgorithmName() const;
    [[nodiscard]] std::uint32_t totalMemoryKB() const;
    void setTotalMemoryKB(std::uint32_t totalMemoryKB);
    [[nodiscard]] std::uint32_t usedMemoryKB() const;
    [[nodiscard]] std::uint32_t freeMemoryKB() const;
    void setAlgorithmDirect(AllocAlgorithm algorithm);
    bool validateBlocks(std::string& message) const;

    [[nodiscard]] std::vector<MemoryBlock> exportBlocks() const;
    void importBlocks(const std::vector<MemoryBlock>& blocks);

private:
    bool allocateLocked(
        const std::string& owner,
        std::uint32_t pid,
        const std::string& tag,
        std::uint32_t sizeKB,
        MemoryBlockType type,
        std::uint32_t& outStart,
        std::string& message);
    [[nodiscard]] std::vector<MemoryBlock>::iterator findFreeBlockLocked(std::uint32_t sizeKB);
    void sortBlocksLocked();
    void mergeFreeBlocksLocked();
    [[nodiscard]] static std::string normalizeAlgorithmName(std::string value);

    mutable std::mutex mutex_;
    std::uint32_t totalMemoryKB_ = 1024;
    AllocAlgorithm algorithm_ = AllocAlgorithm::FIRST_FIT;
    std::vector<MemoryBlock> blocks_;
};

} // namespace oscore
