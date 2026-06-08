#include "app/MenuConsole.h"

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

std::string toLowerAscii(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

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

} // namespace

MenuConsole::MenuConsole(InstanceRole role, CommandExecutor executor)
    : role_(role), executor_(std::move(executor)) {}

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
               << "8. 多实例说明\n"
               << "9. 进入原始命令模式\n"
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
            printMultiInstanceGuide(output);
        } else if (choice == "9") {
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

bool MenuConsole::handleProcessMenu(std::istream& input, std::ostream& output) {
    while (!eof_) {
        output << "\n========== 进程管理 ==========\n"
               << "1. 创建进程\n"
               << "2. 查看进程列表\n"
               << "3. 查看进程详情\n"
               << "4. 查看进程树\n"
               << "5. 阻塞进程\n"
               << "6. 唤醒进程\n"
               << "7. 挂起进程\n"
               << "8. 恢复进程\n"
               << "9. 修改进程优先级\n"
               << "10. 删除进程\n"
               << "11. 查看就绪队列\n"
               << "0. 返回主菜单\n";

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
            const auto size = askRequired(input, output, "请输入分配大小KB：");
            if (eof_) return true;
            if (!size.empty() && execute(output, "alloc " + size)) return true;
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
            const auto content = askRequired(input, output, "请输入文件内容：", true);
            if (eof_) return true;
            if (!content.empty() && execute(output, "write_file " + name + " " + content)) return true;
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

bool MenuConsole::execute(std::ostream& output, const std::string& command) const {
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

bool MenuConsole::confirmExit(std::istream& input, std::ostream& output) {
    std::string answer;
    if (!readLine(input, output, "是否确认退出？输入 y 确认：", answer)) {
        return true;
    }
    const auto normalized = toLowerAscii(trim(answer));
    return normalized == "y" || normalized == "yes";
}

void MenuConsole::printMultiInstanceGuide(std::ostream& output) const {
    output << "\n========== 多实例说明 ==========\n"
           << "- 第一个启动的窗口是 MASTER，负责维护真实内核状态。\n"
           << "- 后续启动的窗口是 CLIENT，命令会通过 Named Pipe 发送给 MASTER。\n"
           << "- CLIENT 输入 exit 只会关闭当前客户端窗口。\n"
           << "- MASTER 输入 exit 才会关闭整个内核。\n"
           << "- 双窗口演示时，可以在一个窗口创建进程，在另一个窗口执行 overview 查看同步状态。\n";
}

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

bool MenuConsole::handleContinuousCreateProcess(std::istream& input, std::ostream& output) {
    output << "\n========== 连续创建进程 ==========\n"
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
