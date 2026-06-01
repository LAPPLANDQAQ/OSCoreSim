#pragma once

#include <string>
#include <vector>

namespace oscore {

enum class ProcessState {
    New,
    Ready,
    Running,
    Blocked,
    SuspendedReady,
    SuspendedBlocked,
    Terminated,
    Swapped
};

struct PCB {
    int pid = 0;
    int ppid = 0;
    std::string name;
    std::string owner;
    ProcessState state = ProcessState::New;
    int priority = 0;
    int queueLevel = 0;
    int totalTime = 0;
    int executedTime = 0;
    int remainingTime = 0;
    int timeSliceLeft = 0;
    int memStart = -1;
    int memSize = 0;
    bool swappedOut = false;
    std::vector<int> children;
};

[[nodiscard]] inline const char* toString(ProcessState state) {
    switch (state) {
    case ProcessState::New:
        return "NEW";
    case ProcessState::Ready:
        return "READY";
    case ProcessState::Running:
        return "RUNNING";
    case ProcessState::Blocked:
        return "BLOCKED";
    case ProcessState::SuspendedReady:
        return "SUSPENDED_READY";
    case ProcessState::SuspendedBlocked:
        return "SUSPENDED_BLOCKED";
    case ProcessState::Terminated:
        return "TERMINATED";
    case ProcessState::Swapped:
        return "SWAPPED";
    }
    return "UNKNOWN";
}

} // namespace oscore
