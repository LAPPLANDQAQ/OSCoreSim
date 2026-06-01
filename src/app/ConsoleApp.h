#pragma once

#include "kernel/Kernel.h"

#include <iosfwd>

namespace oscore {

class ConsoleApp {
public:
    int run();
    int run(std::istream& input, std::ostream& output);

private:
    Kernel kernel_;
};

} // namespace oscore
