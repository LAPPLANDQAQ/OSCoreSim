#include "kernel/CommandDispatcher.h"

#include "util/StringUtil.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace oscore {

namespace {

[[nodiscard]] bool parseUint32Strict(const std::string& text, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }

    unsigned long long parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    value = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool parseIntStrict(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }

    int parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }

    value = parsed;
    return true;
}

} // namespace

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
    UserManager& userManager,
    ProcessManager& processManager,
    MemoryManager& memoryManager) const {
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
        return {true, statusText(context, processManager, memoryManager), false};
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

    if (command.name == "create_pcb") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 4 && command.arguments.size() != 5) {
            return {false, "Usage: create_pcb <name> <memKB> <priority> <totalTime> [ppid]", false};
        }

        std::uint32_t memKB = 0;
        int priority = 0;
        std::uint32_t totalTime = 0;
        std::optional<std::uint32_t> ppid;
        if (!parseUint32Strict(command.arguments[1], memKB) ||
            !parseIntStrict(command.arguments[2], priority) ||
            !parseUint32Strict(command.arguments[3], totalTime)) {
            return {false, "Usage: create_pcb <name> <memKB> <priority> <totalTime> [ppid]", false};
        }
        if (command.arguments.size() == 5) {
            std::uint32_t parsedPpid = 0;
            if (!parseUint32Strict(command.arguments[4], parsedPpid)) {
                return {false, "Usage: create_pcb <name> <memKB> <priority> <totalTime> [ppid]", false};
            }
            ppid = parsedPpid;
        }

        if (command.arguments[0].empty() || memKB == 0 || totalTime == 0 || priority < 0 || priority > 15) {
            return {false, "Usage: create_pcb <name> <memKB> <priority> <totalTime> [ppid]", false};
        }

        const auto owner = userManager.currentUser();
        const auto expectedPid = processManager.nextPid();
        std::uint32_t memStart = 0;
        std::string memoryMessage;
        if (!memoryManager.allocateForProcess(owner, expectedPid, command.arguments[0], memKB, memStart, memoryMessage)) {
            return {false, memoryMessage, false};
        }

        std::uint32_t actualPid = 0;
        std::string message;
        const bool ok = processManager.createProcessWithMemory(
            owner,
            command.arguments[0],
            memKB,
            memStart,
            priority,
            totalTime,
            ppid,
            actualPid,
            message);
        if (!ok) {
            std::string rollbackMessage;
            memoryManager.freeByPid(owner, expectedPid, rollbackMessage);
            return {false, message + "\n[ROLLBACK] " + rollbackMessage, false};
        }
        return {ok, message, false};
    }

    if (command.name == "alloc") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "Usage: alloc <sizeKB>", false};
        }
        std::uint32_t sizeKB = 0;
        if (!parseUint32Strict(command.arguments[0], sizeKB)) {
            return {false, "Usage: alloc <sizeKB>", false};
        }
        std::uint32_t start = 0;
        std::string message;
        const bool ok = memoryManager.allocateManual(userManager.currentUser(), sizeKB, start, message);
        return {ok, message, false};
    }

    if (command.name == "free_mem") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "Usage: free_mem <addr>", false};
        }
        std::uint32_t addr = 0;
        if (!parseUint32Strict(command.arguments[0], addr)) {
            return {false, "Usage: free_mem <addr>", false};
        }
        std::string message;
        const bool ok = memoryManager.freeByAddress(userManager.currentUser(), addr, message);
        return {ok, message, false};
    }

    if (command.name == "show_mem" || command.name == "mem_stat") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (!command.arguments.empty()) {
            return {false, std::string("Usage: ") + command.name, false};
        }
        if (command.name == "show_mem") {
            return {true, memoryManager.showMemory(userManager.currentUser()), false};
        }
        return {true, memoryManager.memoryStat(), false};
    }

    if (command.name == "set_alloc_algo") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "Usage: set_alloc_algo <FF|BF|WF>", false};
        }
        std::string message;
        const bool ok = memoryManager.setAlgorithm(command.arguments[0], message);
        return {ok, message, false};
    }

    if (command.name == "compact") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (!command.arguments.empty()) {
            return {false, "Usage: compact", false};
        }
        auto result = memoryManager.compact();
        for (const auto& [pid, newStart] : result.pidNewStart) {
            processManager.updateProcessMemoryStart(pid, newStart);
        }
        return {result.success, result.message, false};
    }

    if (command.name == "pgfault") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() > 1) {
            return {false, "Usage: pgfault [pid]", false};
        }
        std::ostringstream output;
        if (command.arguments.empty()) {
            output << "[PAGE FAULT] Generic simulated page fault.";
        } else {
            std::uint32_t pid = 0;
            if (!parseUint32Strict(command.arguments[0], pid)) {
                return {false, "Usage: pgfault [pid]", false};
            }
            if (!processManager.hasProcess(userManager.currentUser(), pid)) {
                return {false, "Page fault failed: PID does not exist or access denied.", false};
            }
            output << "[PAGE FAULT] PID=" << pid << " triggered a simulated page fault.";
        }
        output << "\n[HANDLER] Save current context."
               << "\n[HANDLER] Locate missing page."
               << "\n[HANDLER] Simulate loading page into memory."
               << "\n[HANDLER] Restore process context."
               << "\n[OK] Page fault handled.";
        return {true, output.str(), false};
    }

    if (command.name == "swap_out") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "Usage: swap_out <pid>", false};
        }
        std::uint32_t pid = 0;
        if (!parseUint32Strict(command.arguments[0], pid)) {
            return {false, "Usage: swap_out <pid>", false};
        }
        const auto owner = userManager.currentUser();
        if (!processManager.hasProcess(owner, pid)) {
            return {false, "Swap out failed: PID does not exist or access denied.", false};
        }
        if (processManager.isSwappedOut(owner, pid)) {
            return {false, "Swap out failed: process is already swapped out.", false};
        }

        std::string memoryMessage;
        if (!memoryManager.swapOutProcess(owner, pid, memoryMessage)) {
            return {false, memoryMessage, false};
        }
        std::string processMessage;
        const bool ok = processManager.markSwappedOut(owner, pid, processMessage);
        return {ok, memoryMessage + "\n" + processMessage, false};
    }

    if (command.name == "kill_pcb" ||
        command.name == "block_pcb" ||
        command.name == "wakeup_pcb" ||
        command.name == "show_pcb" ||
        command.name == "suspend" ||
        command.name == "resume") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            std::ostringstream usage;
            usage << "Usage: " << command.name << " <pid>";
            return {false, usage.str(), false};
        }

        std::uint32_t pid = 0;
        if (!parseUint32Strict(command.arguments[0], pid)) {
            std::ostringstream usage;
            usage << "Usage: " << command.name << " <pid>";
            return {false, usage.str(), false};
        }

        const auto owner = userManager.currentUser();
        if (command.name == "show_pcb") {
            const auto message = processManager.showProcess(owner, pid);
            return {message.find("access denied") == std::string::npos, message, false};
        }

        std::string message;
        bool ok = false;
        if (command.name == "kill_pcb") {
            std::vector<std::uint32_t> removedPids;
            ok = processManager.killProcess(owner, pid, removedPids, message);
            if (ok) {
                std::ostringstream released;
                for (const auto removedPid : removedPids) {
                    std::string freeMessage;
                    if (memoryManager.freeByPid(owner, removedPid, freeMessage)) {
                        released << '\n' << freeMessage;
                    }
                }
                message += released.str();
            }
        } else if (command.name == "block_pcb") {
            ok = processManager.blockProcess(owner, pid, message);
        } else if (command.name == "wakeup_pcb") {
            ok = processManager.wakeupProcess(owner, pid, message);
        } else if (command.name == "suspend") {
            ok = processManager.suspendProcess(owner, pid, message);
        } else if (command.name == "resume") {
            ok = processManager.resumeProcess(owner, pid, message);
        }
        return {ok, message, false};
    }

    if (command.name == "renice") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 2) {
            return {false, "Usage: renice <pid> <newPriority>", false};
        }

        std::uint32_t pid = 0;
        int newPriority = 0;
        if (!parseUint32Strict(command.arguments[0], pid) || !parseIntStrict(command.arguments[1], newPriority)) {
            return {false, "Usage: renice <pid> <newPriority>", false};
        }

        std::string message;
        const bool ok = processManager.reniceProcess(userManager.currentUser(), pid, newPriority, message);
        return {ok, message, false};
    }

    if (command.name == "list_pcb" || command.name == "ptree" || command.name == "readyq") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (!command.arguments.empty()) {
            std::ostringstream usage;
            usage << "Usage: " << command.name;
            return {false, usage.str(), false};
        }

        const auto owner = userManager.currentUser();
        if (command.name == "list_pcb") {
            return {true, processManager.listProcesses(owner), false};
        }
        if (command.name == "ptree") {
            return {true, processManager.processTree(owner), false};
        }
        return {true, processManager.readyQueueSnapshot(owner), false};
    }

    // TODO(P7+): VFS and IPC commands must also call requireLogin() before touching OS resources.

    if (command.name == "save") {
        return {false, "Persistence command must be handled by Kernel.", false};
    }

    if (command.name == "load") {
        return {false, "Persistence command must be handled by Kernel.", false};
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
           << "  clear   - clear the console area\n"
           << "  exit    - cleanly shut down the simulator\n"
           << "  quit    - cleanly shut down the simulator\n"
           << "\n"
           << "Persistence commands:\n"
           << "  save    Save complete simulator state to binary file\n"
           << "  load    Load complete simulator state from binary file\n"
           << "\n"
           << "User commands:\n"
           << "  register <username> <password>   Register a new user\n"
           << "  login <username> <password>      Login to the simulator\n"
           << "  logout                           Logout current session\n"
           << "  whoami                           Show current login user\n"
           << "\n"
           << "Process commands:\n"
           << "  create_pcb <name> <memKB> <priority> <totalTime> [ppid]\n"
           << "  kill_pcb <pid>\n"
           << "  block_pcb <pid>\n"
           << "  wakeup_pcb <pid>\n"
           << "  show_pcb <pid>\n"
           << "  list_pcb\n"
           << "  ptree\n"
           << "  suspend <pid>\n"
           << "  resume <pid>\n"
           << "  renice <pid> <newPriority>\n"
           << "  readyq\n"
           << "\n"
           << "Memory commands:\n"
           << "  alloc <sizeKB>              Manually allocate kernel memory\n"
           << "  free_mem <addr>             Free manually allocated memory by start address\n"
           << "  show_mem                    Show memory block table and ASCII memory map\n"
           << "  compact                     Compact memory and merge free space\n"
           << "  mem_stat                    Show memory usage and fragmentation statistics\n"
           << "  set_alloc_algo <FF|BF|WF>   Change dynamic allocation algorithm\n"
           << "  pgfault [pid]               Simulate a page fault\n"
           << "  swap_out <pid>              Simulate swapping out a process\n"
           << "\n"
           << "Scheduler commands:\n"
           << "  start_sched     Start automatic MLFQ scheduling\n"
           << "  stop_sched      Stop automatic scheduling\n"
           << "  restart_sched   Restart automatic scheduler\n"
           << "  step            Execute one scheduling step and print decision details\n"
           << "\n"
           << "\n"
           << "Visualization commands:\n"
           << "  overview    Show process tree, memory map, MLFQ queues, and system summary\n"
           << "\n"
           << "Virtual file commands:\n"
           << "  touch_file <name>             Create an empty virtual file\n"
           << "  write_file <name> <content>   Write content to a virtual file\n"
           << "  read_file <name>              Read a virtual file\n"
           << "  ls_file                       List current user's virtual files\n"
           << "  rm_file <name>                Remove a virtual file\n"
           << "\n"
           << "OS feature commands for VFS, persistence, and IPC will be added later.";
    return output.str();
}

std::string CommandDispatcher::statusText(
    const CommandContext& context,
    const ProcessManager& processManager,
    const MemoryManager& memoryManager) const {
    std::ostringstream output;
    output << "=== Kernel Status ===\n"
           << "Worker Thread: " << (context.workerRunning ? "RUNNING" : "STOPPED") << '\n'
           << "Scheduler: " << (context.schedulerRunning ? "RUNNING" : "STOPPED") << '\n'
           << "Scheduler Owner: " << (context.schedulerOwner.empty() ? "<none>" : context.schedulerOwner) << '\n'
           << "Auto Interval: " << context.schedulerIntervalMs << "ms\n"
           << "Snapshot File: " << context.snapshotPath << '\n'
           << "Auto Load: " << context.autoLoadStatus << '\n'
           << "Current User: " << (context.username.empty() ? "<none>" : context.username) << '\n'
           << "Process Count: " << (context.username.empty() ? 0 : processManager.processCount(context.username)) << '\n'
           << processManager.readyQueueSnapshot(context.username) << '\n'
           << "Memory Manager: ENABLED\n"
           << "Total Memory: " << memoryManager.totalMemoryKB() << " KB\n"
           << "Allocation Algorithm: " << memoryManager.currentAlgorithmName() << '\n'
           << "Used: " << memoryManager.usedMemoryKB() << " KB\n"
           << "Free: " << memoryManager.freeMemoryKB() << " KB\n"
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
