#include "app/MenuConsole.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace oscore {
namespace {

std::string trim(const std::string& value) {
    // 先找第一个非空白字符；全空白时直接返回空字符串。
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    // 再找最后一个非空白字符，保留中间内容不变。
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// ASCII 字符串转小写，用于命令名不区分大小写比较。
std::string toLowerAscii(std::string value) {
    for (auto& ch : value) {
        // 命令关键字只使用 ASCII，tolower 前转 unsigned char 可避免负 char 未定义行为。
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// 对 register/login 命令的密码参数做掩码处理，避免密码明文显示在终端。
// 例如 "register alice 123456" → "register alice ******"
std::string maskedCommandForDisplay(const std::string& command) {
    std::istringstream input(command);
    std::string name;
    std::string username;
    // 只读取命令名和用户名，后续密码字段不再展开显示。
    input >> name >> username;

    const auto lowered = toLowerAscii(name);
    if ((lowered == "register" || lowered == "login") && !username.empty()) {
        return name + " " + username + " ******";
    }
    return command;
}

// 验证内存区标签格式：1-32 字符，仅允许字母/数字/下划线/连字符/点号。
bool isValidMemoryTag(const std::string& value) {
    const auto trimmed = trim(value);
    // 菜单层提前校验标签，避免生成明显非法的 alloc 原始命令。
    return !trimmed.empty() &&
           trimmed.size() <= 32 &&
           std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

// 检查字符串是否只包含数字字符（正整数格式校验）。
bool isPositiveIntegerText(const std::string& input) {
    const auto value = trim(input);
    // 这里只检查文本形态，不在菜单层解析成整数，实际范围由后端命令继续校验。
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

// 检查字符串是否只包含 '0' 字符（用于输入校验，拒绝将 0 作为有效输入）。
bool isZeroIntegerText(const std::string& input) {
    const auto value = trim(input);
    // "0"、"00" 这类输入都视为零，便于给出“必须大于 0”的提示。
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return ch == '0';
           });
}

// 检查字符串是否为负数格式（以 '-' 开头且后续全为数字）。
bool isNegativeIntegerText(const std::string& input) {
    const auto value = trim(input);
    // 用文本规则识别负数，目的是给出更具体的输入错误提示。
    return value.size() > 1 &&
           value.front() == '-' &&
           std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

} // namespace

// 构造函数：记录实例角色（MASTER/CLIENT）和命令执行回调。
MenuConsole::MenuConsole(InstanceRole role, CommandExecutor executor)
    : role_(role), executor_(std::move(executor)) {}

// run()：中文数字菜单主循环。
// 交互终端启动后默认进入此菜单，用户选择编号 → 调用对应子菜单 → 子菜单内部拼接原始命令 → 通过 executor_ 执行。
// 返回 MenuOutcome::EnterRawMode 表示用户选择"进入原始命令模式"，
// 返回 MenuOutcome::ExitProgram 表示用户选择"退出程序"。
MenuOutcome MenuConsole::run(std::istream& input, std::ostream& output) {
    // 每次进入主菜单都重置 EOF 标志，保证同一个 MenuConsole 对象可重新运行。
    eof_ = false;

    while (!eof_) {
        output << "\n========== 主菜单 ==========\n"
               << "1. 用户管理\n"
               << "2. 进程管理\n"
               << "3. 内存管理\n"
               << "4. 调度管理\n"
               << "5. 持久化管理\n"
               << "6. 系统总览\n"
               << "7. 虚拟文件系统\n"
               << "8. 进入原始命令模式\n"
               << "0. 退出程序\n";

        std::string choice;
        if (!readChoice(input, output, choice)) {
            return MenuOutcome::ExitProgram;
        }

        if (choice == "1") {
            // 选项 1 进入用户管理子菜单，不在主菜单直接拼接命令。
            if (handleUserMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "2") {
            // 选项 2 进入进程管理子菜单。
            if (handleProcessMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "3") {
            // 选项 3 进入内存管理子菜单。
            if (handleMemoryMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "4") {
            // 选项 4 进入调度管理子菜单。
            if (handleSchedulerMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "5") {
            // 选项 5 进入持久化管理子菜单。
            if (handlePersistenceMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "6") {
            // 选项 6 进入系统总览子菜单。
            if (handleOverviewMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "7") {
            // 选项 7 进入虚拟文件系统子菜单。
            if (handleVfsMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "8") {
            output << "已切换到原始命令模式。\n";
            return MenuOutcome::EnterRawMode;
        } else if (choice == "0") {
            if (!confirmExit(input, output)) {
                output << "已取消退出，返回主菜单。\n";
                continue;
            }
            if (role_ == InstanceRole::MASTER) {
                // MASTER 的退出要通过原始 exit 命令进入 Kernel，保证后台线程和持久化逻辑按统一路径关闭。
                (void)execute(output, "exit");
            } else {
                // CLIENT 没有 Kernel，退出时只关闭本窗口，不能把 exit 转发给 MASTER。
                output << "正在关闭当前客户端窗口。\n";
            }
            return MenuOutcome::ExitProgram;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }

    return MenuOutcome::ExitProgram;
}

// 用户管理子菜单：注册/登录/登出/查看当前用户。
bool MenuConsole::handleUserMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 用户管理 ==========\n"
               << "1. 注册用户\n"
               << "2. 登录用户\n"
               << "3. 退出登录\n"
               << "4. 查看当前用户\n"
               << "0. 返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1" || choice == "2") {
            // 注册和登录都需要用户名与密码，区别只在最终拼接的命令关键字。
            const auto username = askRequired(input, output, "请输入用户名：");
            if (eof_) return true;
            if (username.empty()) continue;
            const auto password = askRequired(input, output, "请输入密码：");
            if (eof_) return true;
            if (password.empty()) continue;
            // 这里不能改变 register/login 命令名，它们会被 CommandDispatcher 精确匹配。
            const auto command = (choice == "1" ? "register " : "login ") + username + " " + password;
            if (execute(output, command)) return true;
        } else if (choice == "3") {
            // logout 不需要额外参数。
            if (execute(output, "logout")) return true;
        } else if (choice == "4") {
            // whoami 查询当前会话用户。
            if (execute(output, "whoami")) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 进程管理子菜单：创建/查看/阻塞/唤醒/挂起/恢复/修改优先级/删除/就绪队列。
bool MenuConsole::handleProcessMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 进程管理 ==========\n"
               << "1.  创建进程\n"
               << "2.  查看进程列表\n"
               << "3.  查看进程详情\n"
               << "4.  查看进程树\n"
               << "5.  阻塞进程\n"
               << "6.  唤醒进程\n"
               << "7.  挂起进程\n"
               << "8.  恢复进程\n"
               << "9.  修改进程优先级\n"
               << "10. 删除进程\n"
               << "11. 查看就绪队列\n"
               << "0.  返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1") {
            // 创建进程字段较多，交给连续创建流程逐项收集。
            if (handleContinuousCreateProcess(input, output)) return true;
        } else if (choice == "2") {
            // list_pcb 展示当前用户可见的 PCB 表。
            if (execute(output, "list_pcb")) return true;
        } else if (choice == "3") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // show_pcb 后面必须拼接用户输入的 PID。
            if (!pid.empty() && execute(output, "show_pcb " + pid)) return true;
        } else if (choice == "4") {
            // ptree 使用父子关系渲染进程树。
            if (execute(output, "ptree")) return true;
        } else if (choice == "5") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // block_pcb 后自动追加进程表，便于观察状态从 READY/RUNNING 变为 BLOCKED。
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "block_pcb " + pid)) return true;
        } else if (choice == "6") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // wakeup_pcb 后自动展示进程表，便于观察 BLOCKED 回到 READY。
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "wakeup_pcb " + pid)) return true;
        } else if (choice == "7") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // suspend 将进程置为挂起态，菜单层只负责发送原始命令。
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "suspend " + pid)) return true;
        } else if (choice == "8") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // resume 恢复挂起进程，后端决定它能否进入就绪队列。
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "resume " + pid)) return true;
        } else if (choice == "9") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (pid.empty()) continue;
            const auto priority = askRequired(input, output, "请输入新的优先级0-15：");
            if (eof_) return true;
            // renice 参数顺序固定为 PID + 新优先级，不能与菜单提示顺序不一致。
            if (!priority.empty() && executeProcessCommandAndShowTable(output, "renice " + pid + " " + priority)) return true;
        } else if (choice == "10") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // kill_pcb 可能递归终止子进程，实际规则由 ProcessManager 控制。
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "kill_pcb " + pid)) return true;
        } else if (choice == "11") {
            // readyq 展示多级反馈队列当前内容。
            if (execute(output, "readyq")) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 内存管理子菜单：手动分配/释放/查看分区/查看统计/紧缩/切换算法/缺页/换出。
bool MenuConsole::handleMemoryMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 内存管理 ==========\n"
               << "1. 手动分配内存\n"
               << "2. 释放手动内存\n"
               << "3. 查看内存分区\n"
               << "4. 查看内存统计\n"
               << "5. 内存紧缩\n"
               << "6. 切换分配算法\n"
               << "7. 模拟缺页中断\n"
               << "8. 换出进程\n"
               << "0. 返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1") {
            // 手动分配需要名称和大小，交给连续流程做输入校验和状态展示。
            if (handleContinuousManualMemoryAllocation(input, output)) return true;
        } else if (choice == "2") {
            const auto addr = askRequired(input, output, "请输入起始地址：");
            if (eof_) return true;
            // free_mem 按起始地址释放手动内存区。
            if (!addr.empty() && execute(output, "free_mem " + addr)) return true;
        } else if (choice == "3") {
            // show_mem 展示分区表和 ASCII 内存图。
            if (execute(output, "show_mem")) return true;
        } else if (choice == "4") {
            // mem_stat 展示使用率和碎片统计。
            if (execute(output, "mem_stat")) return true;
        } else if (choice == "5") {
            // compact 触发内存紧缩，PCB 地址同步由后端完成。
            if (execute(output, "compact")) return true;
        } else if (choice == "6") {
            const auto algo = askRequired(input, output, "请输入分配算法（FF/BF/WF）：");
            if (eof_) return true;
            // set_alloc_algo 后端识别 FF/BF/WF，菜单不能改写算法关键字。
            if (!algo.empty() && execute(output, "set_alloc_algo " + algo)) return true;
        } else if (choice == "7") {
            const auto pid = askOptional(input, output, "请输入PID（可直接回车执行通用缺页）：");
            if (eof_) return true;
            // pgfault 可带 PID，也可不带参数执行通用缺页演示。
            const auto command = pid.empty() ? std::string("pgfault") : "pgfault " + pid;
            if (execute(output, command)) return true;
        } else if (choice == "8") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            // swap_out 模拟把进程换出内存。
            if (!pid.empty() && execute(output, "swap_out " + pid)) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 连续手动分配内存：循环收集"内存区名称 + 大小 KB"，拼接为 alloc 命令执行。
// 每次分配后自动追加 show_mem 和 list_pcb 输出，便于课堂演示观察变化。
// 输入 0 退出循环，输入 1 继续分配。
bool MenuConsole::handleContinuousManualMemoryAllocation(std::istream& input, std::ostream& output) {
    // 与连续创建进程保持同一种菜单风格：输入一组完整字段，执行一次命令，展示状态，再询问是否继续。
    // 菜单层只拼接 alloc 命令，不直接访问 MemoryManager；Master/Client 仍由 executor_ 保持原有路由。
    output << "\n========== 分配内存 ==========\n"
           << "输入内存区信息后将创建一个手动内存区，每次分配后自动显示内存分区和进程表。\n"
           << "手动内存区不会创建 PCB，进程表仅用于对照观察。\n";

    while (!eof_) {
        std::string rawName;
        if (!readLine(input, output, "请输入内存区名称（输入 0 退出）：", rawName)) {
            return true;
        }

        const auto name = trim(rawName);
        // 名称为 0 是菜单约定的退出信号，不会生成 alloc 命令。
        if (name == "0") {
            output << "已退出连续分配流程，返回内存管理菜单。\n";
            return false;
        }
        if (name.empty()) {
            output << "内存区名称不能为空，请重新输入。\n";
            continue;
        }
        if (!isValidMemoryTag(name)) {
            // 标签不合法时在菜单层拦截，避免后端返回更难理解的错误。
            output << "内存区名称只能包含字母、数字、下划线、横线或点号，请重新输入。\n";
            continue;
        }

        std::string rawSize;
        if (!readLine(input, output, "请输入内存大小 KB：", rawSize)) {
            return true;
        }

        const auto size = trim(rawSize);
        // 下面按错误类型分别提示，帮助初学者理解“大小必须是正整数”的含义。
        if (size.empty()) {
            output << "内存大小不能为空，请重新输入。\n";
            continue;
        }
        if (isNegativeIntegerText(size)) {
            output << "内存大小必须大于 0 KB，请重新输入。\n";
            continue;
        }
        if (!isPositiveIntegerText(size)) {
            output << "内存大小必须是正整数 KB，请重新输入。\n";
            continue;
        }
        if (isZeroIntegerText(size)) {
            output << "内存大小必须大于 0 KB，请重新输入。\n";
            continue;
        }

        const auto command = "alloc " + name + " " + size;
        // 输出即将执行的原始命令，方便把中文菜单操作和命令行接口对应起来。
        output << "\n系统将执行命令：\n" << command << '\n';
        if (executeMemoryAllocationAndShowState(output, command)) {
            return true;
        }

        std::string continueChoice;
        if (!readLine(input, output, "\n是否继续分配内存？（输入 1 继续，其他任意键返回）：", continueChoice)) {
            return true;
        }
        // 只有精确输入 1 才继续；其他任何输入都返回内存菜单。
        if (trim(continueChoice) != "1") {
            output << "已返回内存管理菜单。\n";
            return false;
        }
    }

    return true;
}

// 执行内存分配命令后自动展示 show_mem 和 list_pcb，便于课堂观察。
// 原始命令模式和脚本重定向模式不会调用此辅助函数。
bool MenuConsole::executeMemoryAllocationAndShowState(std::ostream& output, const std::string& command) {
    // 自动 show_mem/list_pcb 只是中文菜单的观察便利；原始命令模式和脚本模式不会调用这个辅助函数。
    output << "\n正在执行命令：\n" << maskedCommandForDisplay(command) << '\n';
    // 先执行真正的 alloc 命令。
    const auto result = executor_(command);

    output << "\n命令执行结果：\n";
    if (result.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << result.message << '\n';
    }

    output << "\n========== 当前内存分区 ==========\n";
    // 再执行 show_mem，把分配后的内存布局立即显示出来。
    const auto mapResult = executor_("show_mem");
    if (mapResult.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << mapResult.message << '\n';
    }

    // 任一命令要求退出或发生致命错误时，菜单循环都应终止。
    return result.shouldExit || result.fatalError ||
           mapResult.shouldExit || mapResult.fatalError;
}

// 调度管理子菜单：单步调度/启动自动调度/停止自动调度/重启调度器。
bool MenuConsole::handleSchedulerMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 调度管理 ==========\n"
               << "1. 单步调度\n"
               << "2. 启动自动调度\n"
               << "3. 停止自动调度\n"
               << "4. 重启调度器\n"
               << "0. 返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1") {
            // step 执行一次 MLFQ 调度决策。
            if (execute(output, "step")) return true;
        } else if (choice == "2") {
            // start_sched 启动 Kernel 的后台自动调度循环。
            if (execute(output, "start_sched")) return true;
        } else if (choice == "3") {
            // stop_sched 停止自动调度，但不清空进程状态。
            if (execute(output, "stop_sched")) return true;
        } else if (choice == "4") {
            // restart_sched 先停再启，实际状态处理由 Kernel 完成。
            if (execute(output, "restart_sched")) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 持久化管理子菜单：保存系统状态（save）/加载系统状态（load）。
bool MenuConsole::handlePersistenceMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 持久化管理 ==========\n"
               << "1. 保存系统状态\n"
               << "2. 加载系统状态\n"
               << "0. 返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1") {
            // save 导出整个内核快照并写入默认快照文件。
            if (execute(output, "save")) return true;
        } else if (choice == "2") {
            // load 从默认快照文件恢复状态。
            if (execute(output, "load")) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 系统总览子菜单：查看 overview / 查看 status。
bool MenuConsole::handleOverviewMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 系统总览 ==========\n"
               << "1. 查看系统全局总览\n"
               << "2. 查看系统状态\n"
               << "0. 返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1") {
            // overview 汇总用户、进程、调度、内存、VFS 等状态。
            if (execute(output, "overview")) return true;
        } else if (choice == "2") {
            // status 更偏向当前内核运行状态和后台线程状态。
            if (execute(output, "status")) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 虚拟文件系统子菜单：创建/写入/读取/列出/删除虚拟文件。
// 写文件操作支持多行输入，以单独一行 . 结束。
bool MenuConsole::handleVfsMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 虚拟文件系统 ==========\n"
               << "1. 创建空文件\n"
               << "2. 写入文件\n"
               << "3. 读取文件\n"
               << "4. 列出文件\n"
               << "5. 删除文件\n"
               << "0. 返回主菜单\n";

        std::string choice;
        if (!readChoice(input, output, choice)) return true;

        if (choice == "0") {
            output << "已返回主菜单。\n";
            return false;
        }
        if (choice == "1") {
            const auto name = askRequired(input, output, "请输入文件名：");
            if (eof_) return true;
            // touch_file 创建空文件；同名冲突由 VFS 后端判断。
            if (!name.empty() && execute(output, "touch_file " + name)) return true;
        } else if (choice == "2") {
            const auto name = askRequired(input, output, "请输入文件名：");
            if (eof_) return true;
            if (name.empty()) continue;
            // 多行输入：逐行读取，以单独一行 . 结束
            output << "请输入文件内容，多行输入时以单独一行 . 结束：\n";
            std::ostringstream multiLine;
            std::string line;
            bool firstLine = true;
            while (true) {
                // 多行内容必须逐行读取，不能用 askRequired，因为空行也是合法文件内容的一部分。
                if (!std::getline(input, line)) {
                    output << '\n';
                    eof_ = true;
                    return true;
                }
                if (line == ".") {
                    break;
                }
                if (!firstLine) {
                    // 还原用户输入中的真实换行，后面再统一转义为命令参数。
                    multiLine << '\n';
                }
                multiLine << line;
                firstLine = false;
            }
            const auto rawContent = multiLine.str();
            if (rawContent.empty()) {
                output << "内容为空，操作已取消，返回上一级菜单。\n";
                continue;
            }
            // 将真实换行符等转义后拼接为 write_file 命令
            std::string escapedContent;
            // 最坏情况下每个字符都可能变为两个字节的转义序列，提前 reserve 减少重分配。
            escapedContent.reserve(rawContent.size() * 2);
            for (const char ch : rawContent) {
                // 命令行解析器接收单行文本，因此换行、制表、反斜杠和双引号必须转义。
                switch (ch) {
                case '\n': escapedContent += "\\n"; break;
                case '\r': escapedContent += "\\r"; break;
                case '\t': escapedContent += "\\t"; break;
                case '\\': escapedContent += "\\\\"; break;
                case '"':  escapedContent += "\\\""; break;
                default:   escapedContent.push_back(ch); break;
                }
            }
            // write_file 参数顺序固定为 文件名 + 内容；内容中的 \n 会由后端解析回真实换行。
            execute(output, "write_file " + name + " " + escapedContent);
        } else if (choice == "3") {
            const auto name = askRequired(input, output, "请输入文件名：");
            if (eof_) return true;
            // read_file 读取当前用户拥有的虚拟文件。
            if (!name.empty() && execute(output, "read_file " + name)) return true;
        } else if (choice == "4") {
            // ls_file 列出当前用户的虚拟文件。
            if (execute(output, "ls_file")) return true;
        } else if (choice == "5") {
            const auto name = askRequired(input, output, "请输入文件名：");
            if (eof_) return true;
            // rm_file 删除当前用户的虚拟文件。
            if (!name.empty() && execute(output, "rm_file " + name)) return true;
        } else {
            output << "输入无效，请重新输入。\n";
        }
    }
    return true;
}

// 执行原始命令并打印结果。executor_ 由 ConsoleApp 注入，Master 走 Kernel，Client 走 NamedPipe。
bool MenuConsole::execute(std::ostream& output, const std::string& command) const {
    // executor_ 由 ConsoleApp 注入：Master 走 Kernel::submitCommand，Client 走 NamedPipeClient，菜单层不区分底层通道。
    // 打印时使用 maskedCommandForDisplay，防止 register/login 密码在屏幕上回显。
    output << "\n正在执行命令：" << maskedCommandForDisplay(command) << '\n';
    const auto result = executor_(command);

    output << "命令执行结果：\n";
    if (result.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << result.message << '\n';
    }

    // shouldExit 表示正常退出请求，fatalError 表示底层通道不可继续使用。
    return result.shouldExit || result.fatalError;
}

// 带提示的逐行读取。遇到 EOF 时设置 eof_ 标志。
bool MenuConsole::readLine(
    std::istream& input,
    std::ostream& output,
    const std::string& prompt,
    std::string& line) {
    // 所有菜单输入都通过同一个辅助函数读取，EOF 处理逻辑保持一致。
    output << prompt;
    if (!std::getline(input, line)) {
        output << '\n';
        eof_ = true;
        return false;
    }
    return true;
}

// 读取菜单选项编号（trim 后返回）。
bool MenuConsole::readChoice(std::istream& input, std::ostream& output, std::string& choice) {
    if (!readLine(input, output, "请输入选项编号：", choice)) {
        return false;
    }
    // 菜单编号前后的空白不影响选择。
    choice = trim(choice);
    if (choice.empty()) {
        output << "输入无效，请重新输入。\n";
    }
    return true;
}

// 退出确认：输入 y/yes 确认退出，其他输入取消。
bool MenuConsole::confirmExit(std::istream& input, std::ostream& output) {
    std::string answer;
    if (!readLine(input, output, "是否确认退出？输入 y 确认：", answer)) {
        return true;
    }
    // 退出确认只识别 ASCII y/yes，大小写不敏感。
    const auto normalized = toLowerAscii(trim(answer));
    return normalized == "y" || normalized == "yes";
}

// 必填输入：空输入时取消操作并返回上一级菜单。
std::string MenuConsole::askRequired(
    std::istream& input,
    std::ostream& output,
    const std::string& prompt,
    bool preserveSpaces) {
    std::string value;
    if (!readLine(input, output, prompt, value)) {
        return {};
    }

    // 必填字段会先 trim 判断是否为空；空输入表示取消当前操作。
    const auto trimmed = trim(value);
    if (trimmed.empty()) {
        output << "输入为空，操作已取消，返回上一级菜单。\n";
        return {};
    }
    // preserveSpaces 用于需要保留原始空白的字段；当前菜单大多使用 trim 后的值。
    return preserveSpaces ? value : trimmed;
}

// 连续创建进程：循环收集"名称+内存+优先级+总时间+父PID"，拼接为 create_pcb 命令执行。
// 每次创建后自动追加 list_pcb 显示进程表。输入 0 退出循环。
bool MenuConsole::handleContinuousCreateProcess(std::istream& input, std::ostream& output) {
    output << "\n========== 创建进程 ==========\n"
           << "输入进程信息后将创建一个进程，每次创建后自动显示进程表。\n"
           << "进程名输入 0 可退出连续创建流程。\n";

    while (!eof_) {
        const auto name = askRequired(input, output, "请输入进程名称（输入 0 退出）：");
        if (eof_) return true;
        if (name.empty()) continue;

        // 输入 0 退出连续创建流程
        if (trim(name) == "0") {
            output << "已退出连续创建流程，返回进程管理菜单。\n";
            return false;
        }

        const auto mem = askRequired(input, output, "请输入内存大小 KB：");
        if (eof_) return true;
        if (mem.empty()) continue;

        const auto priority = askRequired(input, output, "请输入优先级 0-15：");
        if (eof_) return true;
        if (priority.empty()) continue;

        const auto totalTime = askRequired(input, output, "请输入总运行时间 tick：");
        if (eof_) return true;
        if (totalTime.empty()) continue;

        const auto ppid = askOptional(input, output, "请输入父进程 PID（可直接回车跳过）：");
        if (eof_) return true;

        std::ostringstream command;
        // create_pcb 参数顺序必须与 CommandDispatcher 保持一致：名称、内存、优先级、总时间、可选父 PID。
        command << "create_pcb " << name << ' ' << mem << ' ' << priority << ' ' << totalTime;
        if (!ppid.empty()) {
            // 父 PID 为空时不拼接第五个参数，让后端按“无父进程”处理。
            command << ' ' << ppid;
        }

        // 创建进程并自动展示进程表
        if (executeProcessCommandAndShowTable(output, command.str())) return true;

        // 询问是否继续
        std::string continueChoice;
        if (!readLine(input, output, "\n是否继续创建进程？（输入 1 继续，其他任意键返回）：", continueChoice)) {
            return true;
        }
        // 只有输入 1 才继续创建；其余输入回到进程管理菜单。
        if (trim(continueChoice) != "1") {
            output << "已返回进程管理菜单。\n";
            return false;
        }
    }
    return true;
}

// 执行进程命令后自动追加 list_pcb 显示当前进程表，便于课堂演示观察进程状态变化。
bool MenuConsole::executeProcessCommandAndShowTable(std::ostream& output, const std::string& command) {
    // 1. 执行原始进程命令
    const bool shouldExit = execute(output, command);

    // 2. 无论原始命令成功与否，追加当前进程表
    output << "\n========== 当前进程表 ==========\n";
    // list_pcb 是只读命令，用于观察刚才的状态变化。
    const auto tableResult = executor_("list_pcb");
    if (!tableResult.message.empty()) {
        output << tableResult.message << '\n';
    } else {
        output << "（无输出）\n";
    }

    return shouldExit || tableResult.fatalError;
}

// 可选输入：允许空输入（直接回车跳过），不取消操作。
std::string MenuConsole::askOptional(
    std::istream& input,
    std::ostream& output,
    const std::string& prompt,
    bool preserveSpaces) {
    std::string value;
    if (!readLine(input, output, prompt, value)) {
        return {};
    }
    // 可选字段允许空字符串，调用方根据空值决定是否拼接对应命令参数。
    return preserveSpaces ? value : trim(value);
}

} // namespace oscore
