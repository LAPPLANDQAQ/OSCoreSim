#include "app/ConsoleApp.h"

#include <iostream>
#include <string>

namespace oscore {

int ConsoleApp::run() {
    return run(std::cin, std::cout);
}

int ConsoleApp::run(std::istream& input, std::ostream& output) {
    output << "Persistent OS Core Simulator\n";
    output << "Type 'help' for commands or 'exit' to quit.\n";

    std::string line;
    while (true) {
        output << "os_sim> ";
        if (!std::getline(input, line)) {
            output << '\n';
            break;
        }

        const auto command = dispatcher_.parse(line);
        const auto result = kernel_.execute(command);
        if (!result.output.empty()) {
            output << result.output << '\n';
        }
        if (result.shouldExit) {
            break;
        }
    }

    return 0;
}

} // namespace oscore
