#pragma once

#include <string>

namespace oscore {

class MemoryManager {
public:
    [[nodiscard]] std::string moduleName() const;
};

} // namespace oscore
