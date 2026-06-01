#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "memory/MemoryManager.h"
#include "persistence/SnapshotStore.h"
#include "process/ProcessManager.h"
#include "process/Scheduler.h"
#include "vfs/VirtualFileSystem.h"

#include <string>

namespace oscore {

struct CommandResult {
    std::string output;
    bool shouldExit = false;
};

class Kernel {
public:
    [[nodiscard]] CommandResult execute(const Command& command);
    [[nodiscard]] std::string helpText() const;

private:
    UserManager users_;
    ProcessManager processes_;
    MemoryManager memory_;
    Scheduler scheduler_;
    SnapshotStore snapshots_;
    VirtualFileSystem vfs_;
};

} // namespace oscore
