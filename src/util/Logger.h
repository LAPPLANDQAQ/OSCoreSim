#pragma once

#include <iostream>
#include <string>

namespace oscore {

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
