#pragma once

#include <string>

namespace oscore {

class ProcessManager {
public:
    [[nodiscard]] std::string moduleName() const;
};

} // namespace oscore
