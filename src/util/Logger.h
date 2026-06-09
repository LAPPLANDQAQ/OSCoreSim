#pragma once

#include <iostream>
#include <string>

namespace oscore {

// 简单日志工具，用于后续课程演示扩展；核心命令仍优先返回字符串而不是深层直接打印。
class Logger {
public:
    static void info(const std::string& message) {
        std::clog << "[info] " << message << '\n';
    }

    static void error(const std::string& message) {
        std::clog << "[error] " << message << '\n';
    }
};

} // namespace oscore
