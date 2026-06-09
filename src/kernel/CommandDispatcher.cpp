#include "kernel/CommandDispatcher.h"

#include "util/StringUtil.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace oscore {

namespace {

[[nodiscard]] std::string allocUsageText() {
    return "Usage: alloc <sizeKB>\nor: alloc <name> <sizeKB>";
}

[[nodiscard]] bool isValidMemoryTag(const std::string& value) {
    if (value.empty() || value.size() > 32) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

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

    // 原始命令只做轻量拆词：命令名统一小写，参数按空白分隔；复杂状态校验放在 dispatch 分支内。
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
        // 进程和内存属于用户态资源，必须登录后才能创建，避免匿名命令污染 PCB/内存表。
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
        // P4 后进程创建必须先申请真实内存；若 PCB 创建失败，再按 PID 回滚内存块，避免泄漏。
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
        // alloc 是手动内存实验命令，只产生 KERNEL/manual 内存块，不创建 PCB。
        // 兼容旧格式 alloc <sizeKB>，新格式 alloc <name> <sizeKB> 仅把 name 写入 MemoryBlock::tag。
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }

        std::string tag = "manual";
        std::string sizeText;
        if (command.arguments.size() == 1) {
            sizeText = command.arguments[0];
        } else if (command.arguments.size() == 2) {
            tag = command.arguments[0];
            sizeText = command.arguments[1];
            if (!isValidMemoryTag(tag)) {
                return {false, allocUsageText(), false};
            }
        } else {
            return {false, allocUsageText(), false};
        }

        std::uint32_t sizeKB = 0;
        if (!parseUint32Strict(sizeText, sizeKB) || sizeKB == 0) {
            return {false, allocUsageText(), false};
        }
        std::uint32_t start = 0;
        std::string message;
        const bool ok = memoryManager.allocateManual(userManager.currentUser(), tag, sizeKB, start, message);
        return {ok, message, false};
    }

    if (command.name == "free_mem") {
        // free_mem 只释放当前用户通过 alloc 得到的手动块；进程块必须走 kill_pcb 或 swap_out。
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
        // 分配算法切换影响后续 allocateLocked 的空闲块选择，不改变现有分区布局。
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
        // 内存紧缩会移动 PROCESS 块，必须同步更新对应 PCB.memStart。
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
                // kill_pcb 会递归删除子树，随后逐个释放子树中所有进程的物理内存。
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

    // P8-P9: save/load 和 VFS/overview 命令均由 Kernel 在 executeRequest 中优先路由处理。
    // CommandDispatcher 仅处理仍在此处分发的通用命令（用户、进程、内存）。

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
    output << "可用命令：\n"
           << "  help    - 显示命令列表\n"
           << "  status  - 显示内核状态摘要\n"
           << "  clear   - 清空当前控制台显示区域\n"
           << "  exit    - 安全退出模拟器\n"
           << "  quit    - 安全退出模拟器\n"
           << "\n"
           << "持久化命令：\n"
           << "  save    将完整系统状态保存到二进制快照文件\n"
           << "  load    从二进制快照文件加载完整系统状态\n"
           << "\n"
           << "用户命令：\n"
           << "  register <username> <password>   注册新用户\n"
           << "  login <username> <password>      登录模拟器\n"
           << "  logout                           退出当前登录\n"
           << "  whoami                           显示当前登录用户\n"
           << "\n"
           << "进程命令：\n"
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
           << "内存命令：\n"
           << "  alloc <sizeKB>              手动分配内存，Tag 默认为 manual\n"
           << "  alloc <name> <sizeKB>       手动分配命名内存区\n"
           << "  free_mem <addr>             按起始地址释放手动分配内存\n"
           << "  show_mem                    显示内存分区表和 ASCII 内存图\n"
           << "  compact                     执行内存紧缩并合并空闲分区\n"
           << "  mem_stat                    显示内存使用率和碎片统计\n"
           << "  set_alloc_algo <FF|BF|WF>   切换动态分区分配算法\n"
           << "  pgfault [pid]               模拟缺页中断\n"
           << "  swap_out <pid>              模拟进程换出\n"
           << "\n"
           << "调度命令：\n"
           << "  start_sched     启动自动 MLFQ 调度\n"
           << "  stop_sched      停止自动调度\n"
           << "  restart_sched   清理就绪队列后重启调度器\n"
           << "  step            执行一次单步调度并打印决策过程\n"
           << "\n"
           << "\n"
           << "可视化命令：\n"
           << "  overview    显示进程树、内存图、MLFQ 队列和系统摘要\n"
           << "\n"
           << "虚拟文件命令：\n"
           << "  touch_file <name>             创建空虚拟文件\n"
           << "  write_file <name> <content>   写入虚拟文件内容\n"
           << "  read_file <name>              读取虚拟文件内容\n"
           << "  ls_file                       列出当前用户的虚拟文件\n"
           << "  rm_file <name>                删除虚拟文件\n"
           << "\n"
           << "VFS、持久化和 IPC 相关命令均已集成到上述命令中。";
    output << "\nAliases:\n"
           << "  start     -> start_sched\n"
           << "  stop      -> stop_sched\n"
           << "  restart   -> restart_sched\n";
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
