#pragma once

#include "kernel/CommandDispatcher.h"
#include "kernel/Kernel.h"

#include <iosfwd>

namespace oscore {

class ConsoleApp {
public:
    int run();
    int run(std::istream& input, std::ostream& output);

private:
    CommandDispatcher dispatcher_;
    Kernel kernel_;
};

} // namespace oscore
