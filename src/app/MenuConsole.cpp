#include "app/MenuConsole.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace oscore {
namespace {

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// ASCII 字符串转小写，用于命令名不区分大小写比较。
std::string toLowerAscii(std::string value) {
    for (auto& ch : value) {
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
    return !trimmed.empty() &&
           trimmed.size() <= 32 &&
           std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

// 检查字符串是否只包含数字字符（正整数格式校验）。
bool isPositiveIntegerText(const std::string& input) {
    const auto value = trim(input);
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

// 检查字符串是否只包含 '0' 字符（用于输入校验，拒绝将 0 作为有效输入）。
bool isZeroIntegerText(const std::string& input) {
    const auto value = trim(input);
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return ch == '0';
           });
}

// 检查字符串是否为负数格式（以 '-' 开头且后续全为数字）。
bool isNegativeIntegerText(const std::string& input) {
    const auto value = trim(input);
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
            if (handleUserMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "2") {
            if (handleProcessMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "3") {
            if (handleMemoryMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "4") {
            if (handleSchedulerMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "5") {
            if (handlePersistenceMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "6") {
            if (handleOverviewMenu(input, output)) return MenuOutcome::ExitProgram;
        } else if (choice == "7") {
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
                (void)execute(output, "exit");
            } else {
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
            const auto username = askRequired(input, output, "请输入用户名：");
            if (eof_) return true;
            if (username.empty()) continue;
            const auto password = askRequired(input, output, "请输入密码：");
            if (eof_) return true;
            if (password.empty()) continue;
            const auto command = (choice == "1" ? "register " : "login ") + username + " " + password;
            if (execute(output, command)) return true;
        } else if (choice == "3") {
            if (execute(output, "logout")) return true;
        } else if (choice == "4") {
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
            if (handleContinuousCreateProcess(input, output)) return true;
        } else if (choice == "2") {
            if (execute(output, "list_pcb")) return true;
        } else if (choice == "3") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (!pid.empty() && execute(output, "show_pcb " + pid)) return true;
        } else if (choice == "4") {
            if (execute(output, "ptree")) return true;
        } else if (choice == "5") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "block_pcb " + pid)) return true;
        } else if (choice == "6") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "wakeup_pcb " + pid)) return true;
        } else if (choice == "7") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "suspend " + pid)) return true;
        } else if (choice == "8") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "resume " + pid)) return true;
        } else if (choice == "9") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (pid.empty()) continue;
            const auto priority = askRequired(input, output, "请输入新的优先级0-15：");
            if (eof_) return true;
            if (!priority.empty() && executeProcessCommandAndShowTable(output, "renice " + pid + " " + priority)) return true;
        } else if (choice == "10") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
            if (!pid.empty() && executeProcessCommandAndShowTable(output, "kill_pcb " + pid)) return true;
        } else if (choice == "11") {
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
            if (handleContinuousManualMemoryAllocation(input, output)) return true;
        } else if (choice == "2") {
            const auto addr = askRequired(input, output, "请输入起始地址：");
            if (eof_) return true;
            if (!addr.empty() && execute(output, "free_mem " + addr)) return true;
        } else if (choice == "3") {
            if (execute(output, "show_mem")) return true;
        } else if (choice == "4") {
            if (execute(output, "mem_stat")) return true;
        } else if (choice == "5") {
            if (execute(output, "compact")) return true;
        } else if (choice == "6") {
            const auto algo = askRequired(input, output, "请输入分配算法（FF/BF/WF）：");
            if (eof_) return true;
            if (!algo.empty() && execute(output, "set_alloc_algo " + algo)) return true;
        } else if (choice == "7") {
            const auto pid = askOptional(input, output, "请输入PID（可直接回车执行通用缺页）：");
            if (eof_) return true;
            const auto command = pid.empty() ? std::string("pgfault") : "pgfault " + pid;
            if (execute(output, command)) return true;
        } else if (choice == "8") {
            const auto pid = askRequired(input, output, "请输入PID：");
            if (eof_) return true;
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
        if (name == "0") {
            output << "已退出连续分配流程，返回内存管理菜单。\n";
            return false;
        }
        if (name.empty()) {
            output << "内存区名称不能为空，请重新输入。\n";
            continue;
        }
        if (!isValidMemoryTag(name)) {
            output << "内存区名称只能包含字母、数字、下划线、横线或点号，请重新输入。\n";
            continue;
        }

        std::string rawSize;
        if (!readLine(input, output, "请输入内存大小 KB：", rawSize)) {
            return true;
        }

        const auto size = trim(rawSize);
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
        output << "\n系统将执行命令：\n" << command << '\n';
        if (executeMemoryAllocationAndShowState(output, command)) {
            return true;
        }

        std::string continueChoice;
        if (!readLine(input, output, "\n是否继续分配内存？（输入 1 继续，其他任意键返回）：", continueChoice)) {
            return true;
        }
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
    const auto result = executor_(command);

    output << "\n命令执行结果：\n";
    if (result.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << result.message << '\n';
    }

    output << "\n========== 当前内存分区 ==========\n";
    const auto mapResult = executor_("show_mem");
    if (mapResult.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << mapResult.message << '\n';
    }

    output << "\n========== 当前进程表 ==========\n";
    const auto processResult = executor_("list_pcb");
    if (processResult.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << processResult.message << '\n';
    }

    return result.shouldExit || result.fatalError ||
           mapResult.shouldExit || mapResult.fatalError ||
           processResult.shouldExit || processResult.fatalError;
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
            if (execute(output, "step")) return true;
        } else if (choice == "2") {
            if (execute(output, "start_sched")) return true;
        } else if (choice == "3") {
            if (execute(output, "stop_sched")) return true;
        } else if (choice == "4") {
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
            if (execute(output, "save")) return true;
        } else if (choice == "2") {
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
            if (execute(output, "overview")) return true;
        } else if (choice == "2") {
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
                if (!std::getline(input, line)) {
                    output << '\n';
                    eof_ = true;
                    return true;
                }
                if (line == ".") {
                    break;
                }
                if (!firstLine) {
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
            escapedContent.reserve(rawContent.size() * 2);
            for (const char ch : rawContent) {
                switch (ch) {
                case '\n': escapedContent += "\\n"; break;
                case '\r': escapedContent += "\\r"; break;
                case '\t': escapedContent += "\\t"; break;
                case '\\': escapedContent += "\\\\"; break;
                case '"':  escapedContent += "\\\""; break;
                default:   escapedContent.push_back(ch); break;
                }
            }
            execute(output, "write_file " + name + " " + escapedContent);
        } else if (choice == "3") {
            const auto name = askRequired(input, output, "请输入文件名：");
            if (eof_) return true;
            if (!name.empty() && execute(output, "read_file " + name)) return true;
        } else if (choice == "4") {
            if (execute(output, "ls_file")) return true;
        } else if (choice == "5") {
            const auto name = askRequired(input, output, "请输入文件名：");
            if (eof_) return true;
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
    output << "\n正在执行命令：" << maskedCommandForDisplay(command) << '\n';
    const auto result = executor_(command);

    output << "命令执行结果：\n";
    if (result.message.empty()) {
        output << "（无输出）\n";
    } else {
        output << result.message << '\n';
    }

    return result.shouldExit || result.fatalError;
}

// 带提示的逐行读取。遇到 EOF 时设置 eof_ 标志。
bool MenuConsole::readLine(
    std::istream& input,
    std::ostream& output,
    const std::string& prompt,
    std::string& line) {
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

    const auto trimmed = trim(value);
    if (trimmed.empty()) {
        output << "输入为空，操作已取消，返回上一级菜单。\n";
        return {};
    }
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
        command << "create_pcb " << name << ' ' << mem << ' ' << priority << ' ' << totalTime;
        if (!ppid.empty()) {
            command << ' ' << ppid;
        }

        // 创建进程并自动展示进程表
        if (executeProcessCommandAndShowTable(output, command.str())) return true;

        // 询问是否继续
        std::string continueChoice;
        if (!readLine(input, output, "\n是否继续创建进程？（输入 1 继续，其他任意键返回）：", continueChoice)) {
            return true;
        }
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
    return preserveSpaces ? value : trim(value);
}

} // namespace oscore
