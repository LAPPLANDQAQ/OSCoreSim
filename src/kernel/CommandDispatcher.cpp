#include "kernel/CommandDispatcher.h"

#include "util/StringUtil.h"

#include <sstream>
#include <string>

namespace oscore {

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

} // namespace oscore
