#pragma once

#include "ipc/InstanceGuard.h"

#include <functional>
#include <iosfwd>
#include <string>

namespace oscore {

// 菜单命令执行结果：message=输出文本 shouldExit=是否退出 fatalError=致命错误
struct MenuCommandResult {
    std::string message;
    bool shouldExit = false;
    bool fatalError = false;
};

// 菜单循环退出原因
enum class MenuOutcome {
    ExitProgram,    // 用户选择退出程序
    EnterRawMode    // 用户选择进入原始命令模式
};

// MenuConsole 只负责“中文数字菜单 -> 原始命令”的映射。
// 真正的命令执行仍然交给 Kernel 或 Named Pipe，避免绕过现有架构。
class MenuConsole {
public:
    using CommandExecutor = std::function<MenuCommandResult(const std::string&)>;

    MenuConsole(InstanceRole role, CommandExecutor executor);

    MenuOutcome run(std::istream& input, std::ostream& output); // 主菜单循环入口

private:
    // 子菜单处理器（各自维护独立的 while 循环）
    bool handleUserMenu(std::istream& input, std::ostream& output);
    bool handleProcessMenu(std::istream& input, std::ostream& output);
    bool handleContinuousCreateProcess(std::istream& input, std::ostream& output);
    bool executeProcessCommandAndShowTable(std::ostream& output, const std::string& command);
    bool handleMemoryMenu(std::istream& input, std::ostream& output);
    bool handleContinuousManualMemoryAllocation(std::istream& input, std::ostream& output);
    bool executeMemoryAllocationAndShowState(std::ostream& output, const std::string& command);
    bool handleSchedulerMenu(std::istream& input, std::ostream& output);
    bool handlePersistenceMenu(std::istream& input, std::ostream& output);
    bool handleOverviewMenu(std::istream& input, std::ostream& output);
    bool handleVfsMenu(std::istream& input, std::ostream& output);

    // 输入/输出辅助
    bool execute(std::ostream& output, const std::string& command) const;
    bool readLine(std::istream& input, std::ostream& output, const std::string& prompt, std::string& line);
    bool readChoice(std::istream& input, std::ostream& output, std::string& choice);
    bool confirmExit(std::istream& input, std::ostream& output);
    [[nodiscard]] std::string askRequired(
        std::istream& input,
        std::ostream& output,
        const std::string& prompt,
        bool preserveSpaces = false);

    [[nodiscard]] std::string askOptional(
        std::istream& input,
        std::ostream& output,
        const std::string& prompt,
        bool preserveSpaces = false);

    InstanceRole role_;          // 当前窗口角色；决定退出命令是否需要转发给 MASTER。
    CommandExecutor executor_;   // 菜单层注入的命令执行器，负责把原始命令送到 Kernel 或 Pipe。
    bool eof_ = false;           // 读取输入流失败后置 true，让各级菜单统一退出。
};

} // namespace oscore
