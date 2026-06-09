#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandTypes.h"
#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"

#include <cstdint>
#include <string>

namespace oscore {

// CommandDispatcher：命令解析与通用命令分发器。
//
// 职责：
//   - parse()：将原始输入行解析为 Command 结构（命令名转小写，按空白拆参数）
//   - dispatch()：根据命令名路由到 UserManager / ProcessManager / MemoryManager 的具体操作
//   - helpText()：生成中文化帮助文本（按模块分组）
//   - statusText()：生成中文化内核状态摘要
//
// 注意：save / load / overview / VFS 命令 / 调度命令 / reset_system 由 Kernel
// 在 executeRequest 中优先拦截处理，不经过此 Dispatcher。
// Dispatcher 仅处理用户、进程、内存等基本管理命令。
class CommandDispatcher {
public:
    [[nodiscard]] Command parse(const std::string& line) const;
    [[nodiscard]] CommandResponse dispatch(
        const Command& command,
        const CommandContext& context,
        UserManager& userManager,
        ProcessManager& processManager,
        MemoryManager& memoryManager) const;

    [[nodiscard]] std::string helpText() const;
    [[nodiscard]] std::string statusText(
        const CommandContext& context,
        const ProcessManager& processManager,
        const MemoryManager& memoryManager) const;

private:
    // 检查用户是否已登录。未登录时返回标准化的提示信息。
    [[nodiscard]] bool requireLogin(const UserManager& userManager, CommandResponse& response) const;
};

} // namespace oscore
