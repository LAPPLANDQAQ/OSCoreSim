#pragma once

#include <iostream>
#include <string>

namespace oscore {

// 简易日志工具。info=普通日志 error=错误日志，均输出到 std::clog。
// 核心命令通过 CommandResponse::message 返回结果，Logger 仅用于辅助调试。
class Logger {
public:
    static void info(const std::string& message) {
        // 普通日志写入 std::clog，不影响命令响应字符串。
        std::clog << "[info] " << message << '\n';
    }

    static void error(const std::string& message) {
        // 错误日志同样写入 std::clog，便于重定向诊断。
        std::clog << "[error] " << message << '\n';
    }
};

} // namespace oscore
