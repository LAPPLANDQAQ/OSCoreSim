#include "app/ConsoleApp.h"
#include "app/MenuConsole.h"

#include <cstdio>
#include <iostream>
#include <string>

#include <io.h>

namespace oscore {

// 便捷入口：使用标准输入输出运行程序。
int ConsoleApp::run(InstanceRole role) {
    // 真实控制台运行时使用标准输入输出；测试可调用另一个重载注入流。
    return run(std::cin, std::cout, role);
}

// 根据角色分发：MASTER 走 masterLoop（持有内核 + Pipe Server），CLIENT 走 clientLoop（管道转发）。
int ConsoleApp::run(std::istream& input, std::ostream& output, InstanceRole role) {
    // InstanceGuard 已经完成主从判断，这里只根据角色进入不同循环。
    if (role == InstanceRole::MASTER) {
        masterLoop(input, output);
    } else {
        clientLoop(input, output);
    }
    return 0;
}

// masterLoop：Master 实例主循环。
// 流程：1.启动 Kernel → 2.启动NamedPipeServer → 3.打印Banner → 4.交互终端显示菜单 → 5.原始命令输入循环 → 6.关闭PipeServer+Kernel
void ConsoleApp::masterLoop(std::istream& input, std::ostream& output) {
    // ===== MASTER 启动流程 =====

    // 1. 启动 Kernel（包括 worker 线程、调度线程、自动加载快照）
    kernel_.start();

    // 2. 启动 Named Pipe Server，让后续 Client 可以连接
    //    Pipe Server 的命令处理器直接调用 Kernel::submitCommand()
    //    使用 RemoteClient 作为命令来源，与本地 Console 区分
    // 启动 Pipe Server；即使启动失败，Master 仍可在本地模式运行
    [[maybe_unused]] const bool pipeStarted = pipeServer_.start([this](const std::string& command) -> std::string {
        // 每个远程客户端请求都被转成 Kernel 命令，和本地原始命令共用同一套执行路径。
        const auto response = kernel_.submitCommand(
            command,
            kernel_.currentUser(),
            CommandSource::RemoteClient);
        // Named Pipe 协议只回传文本，因此这里把 CommandResponse 的 message 取出。
        return response.message;
    });

    // 3. 打印 MASTER 启动 Banner
    output << "========================================\n";
    output << " 可持久化操作系统核心模拟器\n";
    output << " 当前角色：MASTER\n";
    output << " 输入 help 可查看原始命令列表。\n";
    output << "========================================\n";
    const auto startupMessage = kernel_.startupMessage();
    if (!startupMessage.empty()) {
        output << startupMessage << '\n';
    }

    // 交互终端显示中文数字菜单；脚本重定向时自动跳过，保持原始命令模式兼容。
    if (isInteractiveInput(input)) {
        MenuConsole menu(
            InstanceRole::MASTER,
            [this](const std::string& command) -> MenuCommandResult {
                // MASTER 菜单执行命令时直接进入本进程 Kernel。
                const auto response = kernel_.submitCommand(command);
                return {response.message, response.shouldExit, false};
            });

        // 菜单返回 ExitProgram 表示用户已经选择退出；返回 EnterRawMode 则继续落入下方原始命令循环。
        if (menu.run(input, output) == MenuOutcome::ExitProgram) {
            pipeServer_.stop();
            kernel_.stop();
            return;
        }
    }

    // 4. 本地命令输入循环
    std::string line;
    while (true) {
        output << "OS-SIM[MASTER]> ";
        if (!std::getline(input, line)) {
            output << '\n';
            break;
        }

        // 空行不提交给 Kernel，避免产生无意义的“未知命令”输出。
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        // 前台线程只负责读取输入并提交请求，真正执行统一交给 Kernel 后台线程
        const auto response = kernel_.submitCommand(line);
        if (!response.message.empty()) {
            output << response.message << '\n';
        }
        // exit/logout 等命令可通过 shouldExit 通知前台循环停止读取。
        if (response.shouldExit) {
            break;
        }
    }

    // 5. 关闭流程：先停 Pipe Server，再停 Kernel
    //    确保 Client 不会在 Master 关闭过程中尝试提交命令
    pipeServer_.stop();
    kernel_.stop();
}

// clientLoop：Client 实例主循环。
// Client 不创建 Kernel，不读写状态文件。命令通过 NamedPipeClient 转发给 Master，exit 只关闭自身窗口。
void ConsoleApp::clientLoop(std::istream& input, std::ostream& output) {
    // ===== CLIENT 启动流程 =====

    // Client 不创建 Kernel，不启动调度线程，不读写状态文件

    // 打印 CLIENT 启动 Banner
    output << "========================================\n";
    output << " 可持久化操作系统核心模拟器\n";
    output << " 当前角色：CLIENT\n";
    output << " 已通过 Named Pipe 连接到 MASTER 内核。\n";
    output << " 输入 exit 可关闭当前客户端窗口。\n";
    output << "========================================\n";

    // Client 菜单也只负责拼接命令，实际执行仍通过 Named Pipe 转发给 Master。
    if (isInteractiveInput(input)) {
        MenuConsole menu(
            InstanceRole::CLIENT,
            [this](const std::string& command) -> MenuCommandResult {
                std::string response;
                // CLIENT 菜单永远不直接访问 Kernel，只通过管道把原始命令发给 MASTER。
                if (pipeClient_.sendCommand(command, response)) {
                    return {response, false, false};
                }
                // fatalError 让菜单层退出当前客户端窗口，避免继续向不可达 MASTER 发送命令。
                return {response + "\n[INFO] Client will exit because it cannot reach Master.", false, true};
            });

        // CLIENT 菜单选择退出只关闭自身，不会发送 exit 到 MASTER。
        if (menu.run(input, output) == MenuOutcome::ExitProgram) {
            return;
        }
    }

    // 命令输入循环 — 每条命令通过 Named Pipe 发给 Master
    std::string line;
    while (true) {
        output << "OS-SIM[CLIENT]> ";
        if (!std::getline(input, line)) {
            output << '\n';
            break;
        }

        // 重定向脚本中的空行也被跳过，保持原始命令解析器输入干净。
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        // Client 下的 exit/quit 只关闭当前窗口，不发送给 Master
        if (isLocalExitCommand(line)) {
            break;
        }

        // 通过 Named Pipe 发送命令给 Master 并接收响应
        std::string response;
        if (pipeClient_.sendCommand(line, response)) {
            // 管道通信成功时只负责打印 MASTER 返回的文本，不解释命令含义。
            if (!response.empty()) {
                output << response << '\n';
            }
        } else {
            // 通信失败 — 可能是 Master 已关闭
            output << response << "\n"
                   << "[INFO] Client will exit because it cannot reach Master.\n";
            break;
        }
    }
}

// 判断是否为本地的 exit/quit 命令。Client 下这些命令只关闭自身，不发送给 Master。
bool ConsoleApp::isLocalExitCommand(const std::string& line) {
    // 简单判断：去掉前导空格后检查是否为 exit 或 quit
    const auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }

    // 找到命令名（第一个空白前的词）
    const auto end = line.find_first_of(" \t\r\n", start);
    // substr 的第二个参数允许 npos，表示一直取到字符串末尾。
    const auto name = line.substr(start, end - start);

    // 不区分大小写比较
    const auto lowered = [](std::string s) {
        for (auto& ch : s) {
            // 这里故意只处理 ASCII 命令关键字，避免改变中文或 UTF-8 字节序列。
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch + ('a' - 'A'));
            }
        }
        return s;
    };

    const auto cmd = lowered(name);
    return cmd == "exit" || cmd == "quit";
}

// 判断输入源是否为真实交互终端。Windows 下使用 _isatty 检测，脚本重定向时返回 false 从而跳过菜单。
bool ConsoleApp::isInteractiveInput(std::istream& input) {
    // 只有真实 std::cin 且连接到终端时才显示菜单；文件/管道输入保持脚本兼容。
    if (&input != &std::cin) {
        return false;
    }
    // _fileno(stdin) 取 Windows C 运行时文件描述符，_isatty 判断它是否连接到终端。
    return _isatty(_fileno(stdin)) != 0;
}

} // namespace oscore
