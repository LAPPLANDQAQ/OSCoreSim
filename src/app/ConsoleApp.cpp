#include "app/ConsoleApp.h"

#include <iostream>
#include <string>

namespace oscore {

int ConsoleApp::run() {
    return run(std::cin, std::cout);
}

int ConsoleApp::run(std::istream& input, std::ostream& output) {
    kernel_.start();

    output << "========================================\n";
    output << " Persistent OS Core Simulator\n";
    output << " C++20 / Windows / Course Design\n";
    output << " Type 'help' to show available commands.\n";
    output << "========================================\n";

    std::string line;
    while (true) {
        output << "OS-SIM> ";
        if (!std::getline(input, line)) {
            output << '\n';
            break;
        }

        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        // 前台线程只负责读取输入并提交请求，真正执行统一交给 Kernel 后台线程。
        const auto response = kernel_.submitCommand(line);
        if (!response.message.empty()) {
            output << response.message << '\n';
        }
        if (response.shouldExit) {
            break;
        }
    }

    kernel_.stop();
    return 0;
}

} // namespace oscore
