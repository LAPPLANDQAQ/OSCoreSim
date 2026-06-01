#include "kernel/CommandDispatcher.h"

#include "util/StringUtil.h"

#include <sstream>
#include <string>

namespace oscore {

Command CommandDispatcher::parse(const std::string& line) const {
    Command command;
    command.rawLine = line;

    std::istringstream stream(line);
    std::string token;
    if (!(stream >> token)) {
        return command;
    }

    command.name = toLower(token);
    while (stream >> token) {
        command.arguments.push_back(token);
    }

    return command;
}

CommandResponse CommandDispatcher::dispatch(
    const Command& command,
    const CommandContext& context,
    UserManager& userManager) const {
    if (command.empty()) {
        return {true, "", false};
    }

    if (command.name == "help") {
        return {true, helpText(), false};
    }

    if (command.name == "exit" || command.name == "quit") {
        return {true, "Shutting down OS simulator.", true};
    }

    if (command.name == "clear") {
        // 课程演示中保持跨平台：不直接调用 system("cls")，由前端打印空行模拟清屏。
        return {true, std::string(30, '\n'), false};
    }

    if (command.name == "status") {
        return {true, statusText(context), false};
    }

    if (command.name == "register") {
        if (command.arguments.size() != 2) {
            return {false, "Usage: register <username> <password>", false};
        }

        std::string message;
        const bool ok = userManager.registerUser(command.arguments[0], command.arguments[1], message);
        return {ok, message, false};
    }

    if (command.name == "login") {
        if (command.arguments.size() != 2) {
            return {false, "Usage: login <username> <password>", false};
        }

        std::string message;
        const bool ok = userManager.login(command.arguments[0], command.arguments[1], message);
        return {ok, message, false};
    }

    if (command.name == "logout") {
        std::string message;
        const bool ok = userManager.logout(message);
        return {ok, message, false};
    }

    if (command.name == "whoami") {
        return {true, userManager.whoami(), false};
    }

    // TODO(P3+): OS resource commands must call requireLogin() before operating PCB, memory, or VFS.

    if (command.name == "save") {
        return {true, "[TODO] binary save will be implemented in persistence phase", false};
    }

    if (command.name == "load") {
        return {true, "[TODO] binary load will be implemented in persistence phase", false};
    }

    std::ostringstream output;
    output << "Unknown command: " << command.name << '\n'
           << "Type 'help' to show available commands.";
    return {false, output.str(), false};
}

std::string CommandDispatcher::helpText() const {
    std::ostringstream output;
    output << "Available commands:\n"
           << "  help    - show this command list\n"
           << "  status  - show basic kernel status\n"
           << "  save    - placeholder for binary persistence\n"
           << "  load    - placeholder for binary persistence\n"
           << "  clear   - clear the console area\n"
           << "  exit    - cleanly shut down the simulator\n"
           << "  quit    - cleanly shut down the simulator\n"
           << "\n"
           << "User commands:\n"
           << "  register <username> <password>   Register a new user\n"
           << "  login <username> <password>      Login to the simulator\n"
           << "  logout                           Logout current session\n"
           << "  whoami                           Show current login user\n"
           << "\n"
           << "OS feature commands for users, PCB, memory, MLFQ, VFS, persistence, and IPC will be added later.";
    return output.str();
}

std::string CommandDispatcher::statusText(const CommandContext& context) const {
    std::ostringstream output;
    output << "=== Kernel Status ===\n"
           << "Worker Thread: " << (context.workerRunning ? "RUNNING" : "STOPPED") << '\n'
           << "Scheduler: NOT IMPLEMENTED\n"
           << "Current User: " << (context.username.empty() ? "<none>" : context.username) << '\n'
           << "Request ID: " << context.requestId << '\n'
           << "Source: " << (context.source == CommandSource::LocalConsole ? "local console" : "remote client");
    return output.str();
}

bool CommandDispatcher::requireLogin(const UserManager& userManager, CommandResponse& response) const {
    if (userManager.isLoggedIn()) {
        return true;
    }

    response = {false, "This command requires login. Please run: login <username> <password>", false};
    return false;
}

} // namespace oscore
