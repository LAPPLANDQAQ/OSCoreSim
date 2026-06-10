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

// alloc 命令支持两种形式，统一由此函数生成错误提示，避免不同分支提示不一致。
[[nodiscard]] std::string allocUsageText() {
    return "用法：alloc <大小KB>或 alloc <名称> <大小KB>";
}

[[nodiscard]] bool isValidMemoryTag(const std::string& value) {
    // 手动内存区名称不能为空，且长度限制为 32，便于表格显示和快照保存。
    if (value.empty() || value.size() > 32) {
        return false;
    }

    // 只允许 ASCII 字母、数字和少量安全符号，避免命令行解析产生歧义。
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

[[nodiscard]] bool parseUint32Strict(const std::string& text, std::uint32_t& value) {
    // 空字符串不是数字，直接判定失败。
    if (text.empty()) {
        return false;
    }

    unsigned long long parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    // from_chars 不受区域设置影响，适合解析命令行中的纯数字参数。
    const auto result = std::from_chars(first, last, parsed);
    // 必须完整消费字符串，并且数值不能超过 uint32_t 范围。
    if (result.ec != std::errc{} || result.ptr != last || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    // 通过范围检查后再窄化为 uint32_t。
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool parseIntStrict(const std::string& text, int& value) {
    // int 参数用于优先级，允许 from_chars 识别负号，但后续命令会继续检查范围。
    if (text.empty()) {
        return false;
    }

    int parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    // 严格解析要求整段文本都是整数，"12abc" 这类输入会失败。
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
    // rawLine 必须保留，因为 write_file 的内容可能包含空格，不能只依赖 arguments。
    command.rawLine = line;

    std::istringstream stream(line);
    std::string token;
    // 第一个 token 是命令名；如果读取失败，说明输入为空或全是空白。
    if (!(stream >> token)) {
        return command;
    }

    // 命令名统一转小写，使 HELP/help/Help 都能匹配同一个关键字。
    command.name = toLower(token);
    while (stream >> token) {
        // 普通命令参数按空白拆分；需要保留空格的特殊命令由 Kernel 从 rawLine 重新提取。
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
        // help 是纯文本输出命令，不依赖登录状态。
        return {true, helpText(), false};
    }

    if (command.name == "exit" || command.name == "quit") {
        // shouldExit=true 会让 ConsoleApp 和 workerLoop 进入关闭流程。
        return {true, "正在关闭 OS 模拟器。", true};
    }

    if (command.name == "clear") {
        // 用换行模拟清屏，避免调用平台相关清屏 API。
        return {true, std::string(30, '\n'), false};
    }

    if (command.name == "status") {
        // status 只读取 CommandContext 和子系统快照，不修改状态。
        return {true, statusText(context, processManager, memoryManager), false};
    }

    if (command.name == "register") {
        // register 不要求已登录；参数固定为用户名和密码。
        if (command.arguments.size() != 2) {
            return {false, "用法：register <用户名> <密码>", false};
        }

        std::string message;
        // 用户名校验、密码校验、加盐哈希都由 UserManager 负责。
        const bool ok = userManager.registerUser(command.arguments[0], command.arguments[1], message);
        return {ok, message, false};
    }

    if (command.name == "login") {
        // login 会更新 UserManager 的 currentUser_ 会话状态。
        if (command.arguments.size() != 2) {
            return {false, "用法：login <用户名> <密码>", false};
        }

        std::string message;
        const bool ok = userManager.login(command.arguments[0], command.arguments[1], message);
        return {ok, message, false};
    }

    if (command.name == "logout") {
        // logout 清除当前会话；Kernel 会在执行成功后停止自动调度器。
        std::string message;
        const bool ok = userManager.logout(message);
        return {ok, message, false};
    }

    if (command.name == "whoami") {
        // whoami 不改变状态，只返回当前登录用户或未登录提示。
        return {true, userManager.whoami(), false};
    }

    if (command.name == "create_pcb") {
        CommandResponse loginResponse;
        // 创建进程必须绑定到当前用户，因此先检查登录状态。
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
        // 参数含义：进程名、申请内存 KB、优先级、总运行 tick、可选父 PID。
        if (!parseUint32Strict(command.arguments[1], memKB) ||
            !parseIntStrict(command.arguments[2], priority) ||
            !parseUint32Strict(command.arguments[3], totalTime)) {
            return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
        }
        if (command.arguments.size() == 5) {
            std::uint32_t parsedPpid = 0;
            // 父 PID 只有在第五个参数存在时才解析。
            if (!parseUint32Strict(command.arguments[4], parsedPpid)) {
                return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
            }
            ppid = parsedPpid;
        }

        if (command.arguments[0].empty() || memKB == 0 || totalTime == 0 || priority < 0 || priority > 15) {
            // 进程名非空、内存和总时间大于 0、优先级限制在 0-15。
            return {false, "用法：create_pcb <进程名> <内存KB> <优先级> <总时间> [父PID]", false};
        }

        const auto owner = userManager.currentUser();
        // ProcessManager 尚未创建 PCB 前，先读取下一次将使用的 PID，让内存块可绑定同一 PID。
        const auto expectedPid = processManager.nextPid();
        std::uint32_t memStart = 0;
        std::string memoryMessage;
        // 创建 PCB 之前先申请内存；失败时不会产生半成品进程。
        if (!memoryManager.allocateForProcess(owner, expectedPid, command.arguments[0], memKB, memStart, memoryMessage)) {
            return {false, memoryMessage, false};
        }

        std::uint32_t actualPid = 0;
        std::string message;
        // 内存申请成功后再创建 PCB，并把分配得到的 memStart 写入 PCB。
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
            // 如果 PCB 创建失败，必须释放刚才按 expectedPid 申请的内存，避免内存泄漏。
            memoryManager.freeByPid(owner, expectedPid, rollbackMessage);
            return {false, message + "\n" "回滚：" + rollbackMessage, false};
        }
        return {ok, message, false};
    }

    if (command.name == "alloc") {
        CommandResponse loginResponse;
        // 手动内存分配也按用户隔离，未登录不能分配。
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }

        std::string tag = "manual";
        std::string sizeText;
        if (command.arguments.size() == 1) {
            // alloc <大小KB>：使用默认标签 manual。
            sizeText = command.arguments[0];
        } else if (command.arguments.size() == 2) {
            // alloc <名称> <大小KB>：使用用户提供的内存区标签。
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
            // 手动分区大小必须是大于 0 的 uint32。
            return {false, allocUsageText(), false};
        }
        std::uint32_t start = 0;
        std::string message;
        // MemoryManager 根据当前分配算法选择空闲分区并返回起始地址。
        const bool ok = memoryManager.allocateManual(userManager.currentUser(), tag, sizeKB, start, message);
        return {ok, message, false};
    }

    if (command.name == "free_mem") {
        CommandResponse loginResponse;
        // 释放手动内存也必须在当前用户作用域内执行。
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
        // freeByAddress 只释放当前用户拥有的手动分区，不释放进程内存。
        const bool ok = memoryManager.freeByAddress(userManager.currentUser(), addr, message);
        return {ok, message, false};
    }

    if (command.name == "show_mem" || command.name == "mem_stat") {
        CommandResponse loginResponse;
        // 内存查看命令也要求登录，因为 show_mem 会按用户标识区分进程和手动分区。
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (!command.arguments.empty()) {
            return {false, std::string("用法：") + command.name, false};
        }
        if (command.name == "show_mem") {
            // showMemory 输出分区表和 ASCII 内存条。
            return {true, memoryManager.showMemory(userManager.currentUser()), false};
        }
        // memoryStat 输出全局内存使用和碎片统计。
        return {true, memoryManager.memoryStat(), false};
    }

    if (command.name == "set_alloc_algo") {
        CommandResponse loginResponse;
        // 切换算法影响后续分配决策，因此要求处于登录会话。
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() != 1) {
            return {false, "用法：set_alloc_algo <FF|BF|WF>", false};
        }
        std::string message;
        // 算法文本到枚举值的转换由 MemoryManager::setAlgorithm 完成。
        const bool ok = memoryManager.setAlgorithm(command.arguments[0], message);
        return {ok, message, false};
    }

    if (command.name == "compact") {
        CommandResponse loginResponse;
        // 内存紧缩会移动进程分区，必须在登录后执行以保持课程演示流程一致。
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (!command.arguments.empty()) {
            return {false, "用法：compact", false};
        }
        auto result = memoryManager.compact();
        // 紧缩后进程内存起始地址可能改变，必须同步回 PCB.memStart。
        for (const auto& [pid, newStart] : result.pidNewStart) {
            processManager.updateProcessMemoryStart(pid, newStart);
        }
        return {result.success, result.message, false};
    }

    if (command.name == "pgfault") {
        CommandResponse loginResponse;
        // 缺页中断演示属于进程/内存教学功能，要求先登录。
        if (!requireLogin(userManager, loginResponse)) {
            return loginResponse;
        }
        if (command.arguments.size() > 1) {
            return {false, "用法：pgfault [pid]", false};
        }
        std::ostringstream output;
        if (command.arguments.empty()) {
            // 无 PID 时执行通用缺页流程，不检查具体进程。
            output << "缺页中断：通用模拟缺页。";
        } else {
            std::uint32_t pid = 0;
            if (!parseUint32Strict(command.arguments[0], pid)) {
                return {false, "用法：pgfault [pid]", false};
            }
            if (!processManager.hasProcess(userManager.currentUser(), pid)) {
                return {false, "缺页失败：PID 不存在或访问被拒绝。", false};
            }
            // 有 PID 时先确认当前用户有权限访问该进程。
            output << "缺页中断：PID=" << pid << " 触发了模拟缺页。";
        }
        // 下面的文本模拟缺页处理步骤，不改变真实内存状态。
        output << "\n处理器：保存当前上下文。"
               << "\n处理器：定位缺失页面。"
               << "\n处理器：模拟加载页面到内存。"
               << "\n处理器：恢复进程上下文。"
               << "\n[成功] 缺页处理完成。";
        return {true, output.str(), false};
    }

    if (command.name == "swap_out") {
        CommandResponse loginResponse;
        // 换出进程会同时修改 MemoryManager 和 ProcessManager，必须有当前用户。
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
            // 不能换出不存在或不属于当前用户的进程。
            return {false, "换出失败：PID 不存在或访问被拒绝。", false};
        }
        if (processManager.isSwappedOut(owner, pid)) {
            // 已换出的进程不能重复换出。
            return {false, "换出失败：该进程已被换出。", false};
        }

        std::string memoryMessage;
        // 先释放进程占用的内存分区。
        if (!memoryManager.swapOutProcess(owner, pid, memoryMessage)) {
            return {false, memoryMessage, false};
        }
        std::string processMessage;
        // 再把 PCB 标记为换出，形成内存和进程状态的一致结果。
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
        // 这些命令都以单个 PID 为目标，并且只能操作当前用户拥有的进程。
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
            // show_pcb 是只读查询，返回文本中含 access denied 时标记为失败。
            const auto message = processManager.showProcess(owner, pid);
            return {message.find("access denied") == std::string::npos, message, false};
        }

        std::string message;
        bool ok = false;
        if (command.name == "kill_pcb") {
            std::vector<std::uint32_t> removedPids;
            // killProcess 会递归删除目标进程及其子进程，并返回被删除 PID 列表。
            ok = processManager.killProcess(owner, pid, removedPids, message);
            if (ok) {
                std::ostringstream released;
                for (const auto removedPid : removedPids) {
                    std::string freeMessage;
                    // 对每个被删除的进程释放对应 PROCESS 内存块。
                    if (memoryManager.freeByPid(owner, removedPid, freeMessage)) {
                        released << '\n' << freeMessage;
                    }
                }
                message += released.str();
            }
        } else if (command.name == "block_pcb") {
            // block_pcb 将进程从可运行状态移出就绪队列。
            ok = processManager.blockProcess(owner, pid, message);
        } else if (command.name == "wakeup_pcb") {
            // wakeup_pcb 将阻塞进程重新放回对应就绪队列。
            ok = processManager.wakeupProcess(owner, pid, message);
        } else if (command.name == "suspend") {
            // suspend 标记进程挂起，调度器不会选择挂起进程运行。
            ok = processManager.suspendProcess(owner, pid, message);
        } else if (command.name == "resume") {
            // resume 解除挂起，并按状态决定是否回到 READY。
            ok = processManager.resumeProcess(owner, pid, message);
        }
        return {ok, message, false};
    }

    if (command.name == "renice") {
        CommandResponse loginResponse;
        // renice 修改进程优先级和所在反馈队列，必须先登录。
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
        // ProcessManager 内部负责优先级范围校验和 readyQueues_ 更新。
        const bool ok = processManager.reniceProcess(userManager.currentUser(), pid, newPriority, message);
        return {ok, message, false};
    }

    if (command.name == "list_pcb" || command.name == "ptree" || command.name == "readyq") {
        CommandResponse loginResponse;
        // 这些查询都按当前用户过滤进程数据。
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
            // list_pcb 输出表格化 PCB 列表。
            return {true, processManager.listProcesses(owner), false};
        }
        if (command.name == "ptree") {
            // ptree 按父子关系输出树形结构。
            return {true, processManager.processTree(owner), false};
        }
        // readyq 输出 MLFQ 三个就绪队列。
        return {true, processManager.readyQueueSnapshot(owner), false};
    }

    if (command.name == "save") {
        // save 需要 Kernel 汇总所有子系统快照，Dispatcher 不能独立完成。
        return {false, "持久化命令必须由 Kernel 处理。", false};
    }

    if (command.name == "load") {
        // load 会替换所有子系统状态，也必须由 Kernel 统一处理。
        return {false, "持久化命令必须由 Kernel 处理。", false};
    }

    std::ostringstream output;
    // 所有未匹配命令统一给出未知命令提示。
    output << "[错误] 未知命令：" << command.name << '\n'
           << "[提示] 输入 help 查看可用命令。";
    return {false, output.str(), false};
}

std::string CommandDispatcher::helpText() const {
    std::ostringstream output;
    // 帮助文本按课程模块分组，便于从中文菜单切换到原始命令模式后查找命令。
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
    // 返回完整字符串，由 ConsoleApp 统一打印。
    return output.str();
}

std::string CommandDispatcher::statusText(
    const CommandContext& context,
    const ProcessManager& processManager,
    const MemoryManager& memoryManager) const {
    std::ostringstream output;
    // status 汇总的是当前请求执行时的只读状态，不主动刷新或修改子系统。
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
    // 已登录时允许调用方继续执行命令。
    if (userManager.isLoggedIn()) {
        return true;
    }

    // 未登录时写入统一响应，调用方直接 return 即可。
    response = {false, "[提示] 当前命令需要先登录。\n用法：login <用户名> <密码>", false};
    return false;
}

} // namespace oscore
