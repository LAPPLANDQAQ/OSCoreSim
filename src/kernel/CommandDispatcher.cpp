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
    return "用法：alloc <大小KB>或 alloc <名称> <大小KB>";
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
        return {true, "正在关闭 OS 模拟器。", true};
    }

    if (command.name == "clear") {
        return {true, std::string(30, '\n'), false};
    }

    if (command.name == "status") {
        return {true, statusText(context, processManager, memoryManager), false};
    }

    if (command.name == "register") {
        if (command.arguments.size() != 2) {
            return {false, "用法：register <用户名> <密码>", false};
        }

        std::string message;
        const bool ok = userManager.registerUser(command.arguments[0], command.arguments[1], message);
        return {ok, message, false};
    }

    if (command.name == "login") {
        if (command.arguments.size() != 2) {
            return {false, "用法：login <用户名> <密码>", false};
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
            return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
        }

        std::uint32_t memKB = 0;
        int priority = 0;
        std::uint32_t totalTime = 0;
        std::optional<std::uint32_t> ppid;
        if (!parseUint32Strict(command.arguments[1], memKB) ||
            !parseIntStrict(command.arguments[2], priority) ||
            !parseUint32Strict(command.arguments[3], totalTime)) {
            return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
        }
        if (command.arguments.size() == 5) {
            std::uint32_t parsedPpid = 0;
            if (!parseUint32Strict(command.arguments[4], parsedPpid)) {
                return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
            }
            ppid = parsedPpid;
        }

        if (command.arguments[0].empty() || memKB == 0 || totalTime == 0 || priority < 0 || priority > 15) {
            return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
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
            return {false, message + "\n" "回滚：" + rollbackMessage, false};
        }
        return {ok, message, false};
    }

    if (command.name == "alloc") {
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
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "用法：free_mem <地址>", false};
        }
        std::uint32_t addr = 0;
        if (!parseUint32Strict(command.arguments[0], addr)) {
            return {false, "用法：free_mem <地址>", false};
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
            return {false, std::string("用法：") + command.name, false};
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
            return {false, "用法：set_alloc_algo <FF|BF|WF>", false};
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
            return {false, "用法：compact", false};
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
            return {false, "用法：pgfault [pid]", false};
        }
        std::ostringstream output;
        if (command.arguments.empty()) {
            output << "缺页中断：通用模拟缺页。";
        } else {
            std::uint32_t pid = 0;
            if (!parseUint32Strict(command.arguments[0], pid)) {
                return {false, "用法：pgfault [pid]", false};
            }
            if (!processManager.hasProcess(userManager.currentUser(), pid)) {
                return {false, "缺页失败：PID 不存在或访问被拒绝。", false};
            }
            output << "缺页中断：PID=" << pid << " 触发了模拟缺页。";
        }
        output << "\n处理器：保存当前上下文。"
               << "\n处理器：定位缺失页面。"
               << "\n处理器：模拟加载页面到内存。"
               << "\n处理器：恢复进程上下文。"
               << "\n[成功] 缺页处理完成。";
        return {true, output.str(), false};
    }

    if (command.name == "swap_out") {
        CommandResponse loginResponse;
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "用法：swap_out <pid>", false};
        }
        std::uint32_t pid = 0;
        if (!parseUint32Strict(command.arguments[0], pid)) {
            return {false, "用法：swap_out <pid>", false};
        }
        const auto owner = userManager.currentUser();
        if (!processManager.hasProcess(owner, pid)) {
            return {false, "换出失败：PID 不存在或访问被拒绝。", false};
        }
        if (processManager.isSwappedOut(owner, pid)) {
            return {false, "换出失败：该进程已被换出。", false};
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
            usage << "用法：" << command.name << " <pid>";
            return {false, usage.str(), false};
        }

        std::uint32_t pid = 0;
        if (!parseUint32Strict(command.arguments[0], pid)) {
            std::ostringstream usage;
            usage << "用法：" << command.name << " <pid>";
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
            return {false, "用法：renice <pid> <新优先级>", false};
        }

        std::uint32_t pid = 0;
        int newPriority = 0;
        if (!parseUint32Strict(command.arguments[0], pid) || !parseIntStrict(command.arguments[1], newPriority)) {
            return {false, "用法：renice <pid> <新优先级>", false};
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
            usage << "用法：" << command.name;
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

    if (command.name == "save") {
        return {false, "持久化命令必须由 Kernel 处理。", false};
    }

    if (command.name == "load") {
        return {false, "持久化命令必须由 Kernel 处理。", false};
    }

    std::ostringstream output;
    output << "[错误] 未知命令：" << command.name << '\n'
           << "[提示] 输入 help 查看可用命令。";
    return {false, output.str(), false};
}

std::string CommandDispatcher::helpText() const {
    std::ostringstream output;
    output << "可用命令：\n\n"

           << "【系统命令】\n"
           << "  help       显示命令列表\n"
           << "  status     显示内核状态摘要\n"
           << "  clear      清空当前控制台显示区域\n"
           << "  exit       安全退出模拟器\n"
           << "  quit       安全退出模拟器\n"
           << "  reset_system 重置系统到干净状态\n"

           << "\n【用户命令】\n"
           << "  register <用户名> <密码>   注册新用户\n"
           << "  login <用户名> <密码>      登录系统\n"
           << "  logout                      退出当前账号\n"
           << "  whoami                      显示当前账号\n"

           << "\n【进程命令】\n"
           << "  create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]\n"
           << "  kill_pcb <pid>\n"
           << "  block_pcb <pid>\n"
           << "  wakeup_pcb <pid>\n"
           << "  show_pcb <pid>\n"
           << "  list_pcb\n"
           << "  ptree\n"
           << "  suspend <pid>\n"
           << "  resume <pid>\n"
           << "  renice <pid> <新优先级>\n"
           << "  readyq\n"

           << "\n【内存命令】\n"
           << "  alloc <大小KB>              手动分配内存，Tag 默认为 manual\n"
           << "  alloc <名称> <大小KB>       手动分配命名内存区\n"
           << "  free_mem <地址>            按起始地址释放手动分配内存\n"
           << "  show_mem                   显示内存分区表和 ASCII 内存图\n"
           << "  compact                    执行内存紧缩并合并空闲分区\n"
           << "  mem_stat                   显示内存使用率和碎片统计\n"
           << "  set_alloc_algo <FF|BF|WF>  切换动态分区分配算法\n"
           << "  pgfault [pid]              模拟缺页中断\n"
           << "  swap_out <pid>             模拟进程换出\n"

           << "\n【调度命令】\n"
           << "  step            执行一次单步调度并打印决策过程\n"
           << "  start_sched     启动自动 MLFQ 调度\n"
           << "  stop_sched      停止自动调度\n"
           << "  restart_sched   清理就绪队列后重启调度器\n"

           << "\n【持久化命令】\n"
           << "  save    将完整系统状态保存到二进制快照文件\n"
           << "  load    从二进制快照文件加载完整系统状态\n"

           << "\n【可视化命令】\n"
           << "  overview    显示进程树、内存图、MLFQ 队列和系统摘要\n"

           << "\n【虚拟文件命令】\n"
           << "  touch_file <文件名>             创建空虚拟文件（支持中文文件名）\n"
           << "  write_file <文件名> <内容>   写入内容，支持\\n \\r \\t \\\\ \\\" 转义\n"
           << "  read_file <文件名>             读取文件内容\n"
           << "  ls_file                      列出当前用户的虚拟文件\n"
           << "  rm_file <文件名>             删除虚拟文件\n";

    output << "\n别名：\n"
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
    output << "=== 内核状态 / Kernel Status ===\n"
           << "工作线程     : " << (context.workerRunning ? "运行中" : "已停止") << '\n'
           << "调度器       : " << (context.schedulerRunning ? "运行中" : "已停止") << '\n'
           << "调度用户     : " << (context.schedulerOwner.empty() ? "无" : context.schedulerOwner) << '\n'
           << "自动间隔     : " << context.schedulerIntervalMs << " ms\n"
           << "快照文件     : " << context.snapshotPath << '\n'
           << "自动加载     : " << context.autoLoadStatus << '\n'
           << "当前用户     : " << (context.username.empty() ? "无" : context.username) << '\n'
           << "进程数量     : " << (context.username.empty() ? 0 : processManager.processCount(context.username)) << '\n'
           << processManager.readyQueueSnapshot(context.username) << '\n'
           << "内存管理器   : 已启用\n"
           << "内存总量     : " << memoryManager.totalMemoryKB() << " KB\n"
           << "分配算法     : " << memoryManager.currentAlgorithmName() << '\n'
           << "已用内存     : " << memoryManager.usedMemoryKB() << " KB\n"
           << "空闲内存     : " << memoryManager.freeMemoryKB() << " KB\n"
           << "请求编号     : " << context.requestId << '\n'
           << "请求来源     : " << (context.source == CommandSource::LocalConsole ? "本地控制台" : "远程客户端");
    return output.str();
}

bool CommandDispatcher::requireLogin(const UserManager& userManager, CommandResponse& response) const {
    if (userManager.isLoggedIn()) {
        return true;
    }

    response = {false, "[提示] 当前命令需要先登录。\n用法：login <用户名> <密码>", false};
    return false;
}

} // namespace oscore
