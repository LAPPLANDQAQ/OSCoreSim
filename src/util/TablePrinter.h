#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace oscore {

// 预留的轻量表格输出工具，目前只提供逐行输出，避免引入第三方表格库。
class TablePrinter {
public:
    static void printLines(std::ostream& output, const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            output << line << '\n';
        }
    }
};

} // namespace oscore
