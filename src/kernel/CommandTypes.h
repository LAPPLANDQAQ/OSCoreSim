#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace oscore {

enum class CommandSource {
    LocalConsole,
    RemoteClient
};

struct Command {
    std::string rawLine;
    std::string name;
    std::vector<std::string> arguments;

    [[nodiscard]] bool empty() const {
        return name.empty();
    }
};

struct CommandRequest {
    std::uint64_t id = 0;
    std::string rawLine;
    std::string username;
    CommandSource source = CommandSource::LocalConsole;
    std::shared_ptr<std::promise<struct CommandResponse>> promise;
};

struct CommandResponse {
    bool success = false;
    std::string message;
    bool shouldExit = false;
};

struct CommandContext {
    std::uint64_t requestId = 0;
    std::string username;
    CommandSource source = CommandSource::LocalConsole;
    bool workerRunning = false;
};

} // namespace oscore
