#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace oscore {

class TablePrinter {
public:
    static void printLines(std::ostream& output, const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            output << line << '\n';
        }
    }
};

} // namespace oscore
