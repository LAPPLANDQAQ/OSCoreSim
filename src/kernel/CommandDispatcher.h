#pragma once

#include <string>
#include <vector>

namespace oscore {

struct Command {
    std::string rawLine;
    std::string name;
    std::vector<std::string> arguments;

    [[nodiscard]] bool empty() const {
        return name.empty();
    }
};

class CommandDispatcher {
public:
    [[nodiscard]] Command parse(const std::string& line) const;
};

} // namespace oscore
