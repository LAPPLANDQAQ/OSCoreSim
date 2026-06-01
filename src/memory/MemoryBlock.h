#pragma once

#include <string>

namespace oscore {

enum class MemoryBlockType {
    Free,
    Kernel,
    Process
};

struct MemoryBlock {
    int start = 0;
    int size = 0;
    MemoryBlockType type = MemoryBlockType::Free;
    int pid = 0;
    std::string owner;
    std::string tag;
};

} // namespace oscore
