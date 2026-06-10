#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace oscore {

// 轻量表格输出：逐行打印字符串列表。
// 项目中实际表格渲染（进程列表、内存分区表）使用 padRightDisplayWidth() 对齐，本类保留供扩展。
class TablePrinter {
public:
    static void printLines(std::ostream& output, const std::vector<std::string>& lines) {
        // 逐行输出调用方已经格式化好的文本。
        for (const auto& line : lines) {
            output << line << '\n';
        }
    }
};

} // namespace oscore
