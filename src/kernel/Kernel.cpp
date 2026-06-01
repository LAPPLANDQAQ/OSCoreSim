#include "kernel/Kernel.h"

#include <sstream>

namespace oscore {

CommandResult Kernel::execute(const Command& command) {
    if (command.empty()) {
        return {};
    }

    if (command.name == "help") {
        return {helpText(), false};
    }

    if (command.name == "exit") {
        return {"Exiting OS simulator.", true};
    }

    std::ostringstream output;
    output << "Unknown command: " << command.name << '\n'
           << "Type 'help' to show available commands.";
    return {output.str(), false};
}

std::string Kernel::helpText() const {
    std::ostringstream output;
    output << "Available commands:\n"
           << "  help  - show this command list\n"
           << "  exit  - quit the simulator\n"
           << "\n"
           << "Planned modules: auth, process, memory, scheduler, persistence, IPC, VFS.";
    return output.str();
}

} // namespace oscore
