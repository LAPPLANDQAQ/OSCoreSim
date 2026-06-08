#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandTypes.h"
#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"

#include <string>

namespace oscore {

// CommandDispatcher 负责命令解析和通用命令分发。
// 持久化、overview、VFS 和调度线程控制由 Kernel 优先处理，避免 Dispatcher 直接管理跨模块状态。
class CommandDispatcher {
public:
    [[nodiscard]] Command parse(const std::string& line) const;
    [[nodiscard]] CommandResponse dispatch(
        const Command& command,
        const CommandContext& context,
        UserManager& userManager,
        ProcessManager& processManager,
        MemoryManager& memoryManager) const;

private:
    [[nodiscard]] std::string helpText() const;
    [[nodiscard]] std::string statusText(
        const CommandContext& context,
        const ProcessManager& processManager,
        const MemoryManager& memoryManager) const;
    [[nodiscard]] bool requireLogin(const UserManager& userManager, CommandResponse& response) const;
};

} // namespace oscore
