#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oscore {

enum class ProcessState {
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    SUSPENDED_READY,
    SUSPENDED_BLOCKED,
    TERMINATED,
    SWAPPED
};

// PCB 进程控制块保存调度、父子关系、内存占用和换出状态。
// 该结构保持为简单字段，便于 SnapshotStore 显式二进制序列化。
struct PCB {
    std::uint32_t pid = 0;
    std::uint32_t ppid = 0;
    std::string name;
    std::string owner;
    ProcessState state = ProcessState::NEW;
    int priority = 0;
    int queueLevel = 0;
    std::uint32_t totalTime = 0;
    std::uint32_t executedTime = 0;
    std::uint32_t remainingTime = 0;
    std::uint32_t timeSliceLeft = 0;
    std::uint32_t memStart = 0;
    std::uint32_t memSize = 0;
    bool swappedOut = false;
    std::vector<std::uint32_t> children;
};

[[nodiscard]] inline const char* toString(ProcessState state) {
    switch (state) {
    case ProcessState::NEW:
        return "NEW";
    case ProcessState::READY:
        return "READY";
    case ProcessState::RUNNING:
        return "RUNNING";
    case ProcessState::BLOCKED:
        return "BLOCKED";
    case ProcessState::SUSPENDED_READY:
        return "SUSPENDED_READY";
    case ProcessState::SUSPENDED_BLOCKED:
        return "SUSPENDED_BLOCKED";
    case ProcessState::TERMINATED:
        return "TERMINATED";
    case ProcessState::SWAPPED:
        return "SWAPPED";
    }
    return "UNKNOWN";
}

} // namespace oscore
