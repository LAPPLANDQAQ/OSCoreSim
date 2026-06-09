#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace oscore {

// 动态分区分配算法枚举。
// FF: 首次适应 — 选择第一个足够大的空闲块，速度最快
// BF: 最佳适应 — 选择最小的足够大的空闲块，减少内部碎片
// WF: 最差适应 — 选择最大的空闲块，试图保留较大剩余空间
enum class AllocAlgorithm {
    FIRST_FIT,
    BEST_FIT,
    WORST_FIT
};

// 内存块类型枚举。
// FREE:    空闲块，可被分配
// PROCESS: 进程内存块，与 PCB 绑定，通过 kill_pcb / swap_out 释放
// KERNEL:  手动分配块（alloc 命令），用于课程实验演示
// SWAPPED: 已换出块（当前未使用，SWAPPED 类型已废弃，换出后直接变为 FREE）
enum class MemoryBlockType {
    FREE,
    PROCESS,
    KERNEL,
    SWAPPED
};

// MemoryBlock：动态分区中的连续物理内存块，单位为 KB。
// blocks_ 向量按 start 升序排列，所有块首尾相接覆盖 [0, totalMemoryKB)。
// FREE 块必须保持 pid=0、owner=""、tag=""。
// PROCESS 块的 pid 对应该进程 PCB，KERNEL 块的 pid=0。
struct MemoryBlock {
    std::uint32_t start = 0;           // 起始地址（KB 偏移）
    std::uint32_t size = 0;            // 块大小（KB）
    MemoryBlockType type = MemoryBlockType::FREE;
    std::uint32_t pid = 0;             // 所属进程 PID（PROCESS 块专用，FREE/KERNEL 为 0）
    std::string owner;                  // 所属用户名
    std::string tag;                    // 标签名（show_mem 的 Tag 列），manual 或进程名
};

// compact（内存紧缩）操作的结果。
// 紧缩将所有已分配块移到低地址端，消除外部碎片。
// pidNewStart 记录每个 PROCESS 块的新起始地址，Kernel 据此回写 PCB::memStart。
struct CompactionResult {
    bool success = false;
    std::string message;
    std::unordered_map<std::uint32_t, std::uint32_t> pidNewStart;
};

[[nodiscard]] inline const char* toString(MemoryBlockType type) {
    switch (type) {
    case MemoryBlockType::FREE:    return "FREE";
    case MemoryBlockType::PROCESS: return "PROCESS";
    case MemoryBlockType::KERNEL:  return "KERNEL";
    case MemoryBlockType::SWAPPED: return "SWAPPED";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline const char* toString(AllocAlgorithm algorithm) {
    switch (algorithm) {
    case AllocAlgorithm::FIRST_FIT: return "FIRST_FIT";
    case AllocAlgorithm::BEST_FIT:  return "BEST_FIT";
    case AllocAlgorithm::WORST_FIT: return "WORST_FIT";
    }
    return "UNKNOWN";
}

} // namespace oscore
