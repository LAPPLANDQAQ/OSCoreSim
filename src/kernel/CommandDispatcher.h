#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandTypes.h"

#include <string>

namespace oscore {

class CommandDispatcher {
public:
    [[nodiscard]] Command parse(const std::string& line) const;
    [[nodiscard]] CommandResponse dispatch(
        const Command& command,
        const CommandContext& context,
        UserManager& userManager) const;

private:
    [[nodiscard]] std::string helpText() const;
    [[nodiscard]] std::string statusText(const CommandContext& context) const;
    [[nodiscard]] bool requireLogin(const UserManager& userManager, CommandResponse& response) const;
};

} // namespace oscore
