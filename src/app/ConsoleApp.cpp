#include "app/ConsoleApp.h"

#include <iostream>
#include <string>

namespace oscore {

int ConsoleApp::run(InstanceRole role) {
    return run(std::cin, std::cout, role);
}

int ConsoleApp::run(std::istream& input, std::ostream& output, InstanceRole role) {
    if (role == InstanceRole::MASTER) {
        masterLoop(input, output);
    } else {
        clientLoop(input, output);
    }
    return 0;
}

void ConsoleApp::masterLoop(std::istream& input, std::ostream& output) {
    // ===== MASTER 启动流程 =====

    // 1. 启动 Kernel（包括 worker 线程、调度线程、自动加载快照）
    kernel_.start();

    // 2. 启动 Named Pipe Server，让后续 Client 可以连接
    //    Pipe Server 的命令处理器直接调用 Kernel::submitCommand()
    //    使用 RemoteClient 作为命令来源，与本地 Console 区分
    // 启动 Pipe Server；即使启动失败，Master 仍可在本地模式运行
    [[maybe_unused]] const bool pipeStarted = pipeServer_.start([this](const std::string& command) -> std::string {
        const auto response = kernel_.submitCommand(
            command,
            kernel_.currentUser(),
            CommandSource::RemoteClient);
        return response.message;
    });

    // 3. 打印 MASTER 启动 Banner
    output << "========================================\n";
    output << " Persistent OS Core Simulator\n";
    output << " C++20 / Windows / Course Design\n";
    output << " Role: MASTER\n";
    output << " Type 'help' to show available commands.\n";
    output << "========================================\n";
    const auto startupMessage = kernel_.startupMessage();
    if (!startupMessage.empty()) {
        output << startupMessage << '\n';
    }

    // 4. 本地命令输入循环
    std::string line;
    while (true) {
        output << "OS-SIM[MASTER]> ";
        if (!std::getline(input, line)) {
            output << '\n';
            break;
        }

        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        // 前台线程只负责读取输入并提交请求，真正执行统一交给 Kernel 后台线程
        const auto response = kernel_.submitCommand(line);
        if (!response.message.empty()) {
            output << response.message << '\n';
        }
        if (response.shouldExit) {
            break;
        }
    }

    // 5. 关闭流程：先停 Pipe Server，再停 Kernel
    //    确保 Client 不会在 Master 关闭过程中尝试提交命令
    pipeServer_.stop();
    kernel_.stop();
}

void ConsoleApp::clientLoop(std::istream& input, std::ostream& output) {
    // ===== CLIENT 启动流程 =====

    // Client 不创建 Kernel，不启动调度线程，不读写状态文件

    // 打印 CLIENT 启动 Banner
    output << "========================================\n";
    output << " Persistent OS Core Simulator\n";
    output << " C++20 / Windows / Course Design\n";
    output << " Role: CLIENT\n";
    output << " Connected to Kernel Master through Named Pipe.\n";
    output << " Type 'exit' to close this client window.\n";
    output << "========================================\n";

    // 命令输入循环 — 每条命令通过 Named Pipe 发给 Master
    std::string line;
    while (true) {
        output << "OS-SIM[CLIENT]> ";
        if (!std::getline(input, line)) {
            output << '\n';
            break;
        }

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

bool ConsoleApp::isLocalExitCommand(const std::string& line) {
    // 简单判断：去掉前导空格后检查是否为 exit 或 quit
    const auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }

    // 找到命令名（第一个空白前的词）
    const auto end = line.find_first_of(" \t\r\n", start);
    const auto name = line.substr(start, end - start);

    // 不区分大小写比较
    const auto lowered = [](std::string s) {
        for (auto& ch : s) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch + ('a' - 'A'));
            }
        }
        return s;
    };

    const auto cmd = lowered(name);
    return cmd == "exit" || cmd == "quit";
}

} // namespace oscore
