#pragma once

#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"

#include <string>

namespace oscore {

class Scheduler {
public:
    Scheduler() = default;

    [[nodiscard]] std::string step(
        const std::string& owner,
        ProcessManager& processManager,
        MemoryManager& memoryManager);

    [[nodiscard]] bool isRunning() const;
    void setRunning(bool running);

private:
    [[nodiscard]] int quantumForQueue(int queueLevel) const;
    [[nodiscard]] std::string queueName(int queueLevel) const;

    bool running_ = false;
};

} // namespace oscore
