#pragma once

#include "ipc/InstanceGuard.h"

#include <functional>
#include <iosfwd>
#include <string>

namespace oscore {

struct MenuCommandResult {
    std::string message;
    bool shouldExit = false;
    bool fatalError = false;
};

enum class MenuOutcome {
    ExitProgram,
    EnterRawMode
};

// MenuConsole 只负责“中文数字菜单 -> 原始命令”的映射。
// 真正的命令执行仍然交给 Kernel 或 Named Pipe，避免绕过现有架构。
class MenuConsole {
public:
    using CommandExecutor = std::function<MenuCommandResult(const std::string&)>;

    MenuConsole(InstanceRole role, CommandExecutor executor);

    MenuOutcome run(std::istream& input, std::ostream& output);

private:
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

    InstanceRole role_;
    CommandExecutor executor_;
    bool eof_ = false;
};

} // namespace oscore
