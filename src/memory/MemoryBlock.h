#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace oscore {

enum class AllocAlgorithm {
    FIRST_FIT,
    BEST_FIT,
    WORST_FIT
};

enum class MemoryBlockType {
    FREE,
    PROCESS,
    KERNEL,
    SWAPPED
};

// MemoryBlock 表示动态分区中的连续物理内存块，单位为 KB。
// PROCESS 块关联 PCB，KERNEL 块用于手动 alloc 演示，FREE 块必须保持 pid=0 且 owner 为空。
struct MemoryBlock {
    std::uint32_t start = 0;
    std::uint32_t size = 0;
    MemoryBlockType type = MemoryBlockType::FREE;
    std::uint32_t pid = 0;
    std::string owner;
    std::string tag;
};

// compact 后需要把进程内存新地址同步回 PCB，因此返回 pid -> newStart 映射。
struct CompactionResult {
    bool success = false;
    std::string message;
    std::unordered_map<std::uint32_t, std::uint32_t> pidNewStart;
};

[[nodiscard]] inline const char* toString(MemoryBlockType type) {
    switch (type) {
    case MemoryBlockType::FREE:
        return "FREE";
    case MemoryBlockType::PROCESS:
        return "PROCESS";
    case MemoryBlockType::KERNEL:
        return "KERNEL";
    case MemoryBlockType::SWAPPED:
        return "SWAPPED";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline const char* toString(AllocAlgorithm algorithm) {
    switch (algorithm) {
    case AllocAlgorithm::FIRST_FIT:
        return "FIRST_FIT";
    case AllocAlgorithm::BEST_FIT:
        return "BEST_FIT";
    case AllocAlgorithm::WORST_FIT:
        return "WORST_FIT";
    }
    return "UNKNOWN";
}

} // namespace oscore
