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

struct MemoryBlock {
    std::uint32_t start = 0;
    std::uint32_t size = 0;
    MemoryBlockType type = MemoryBlockType::FREE;
    std::uint32_t pid = 0;
    std::string owner;
    std::string tag;
};

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
