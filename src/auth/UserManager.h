#pragma once

#include <string>

namespace oscore {

class UserManager {
public:
    [[nodiscard]] std::string moduleName() const;
};

} // namespace oscore
