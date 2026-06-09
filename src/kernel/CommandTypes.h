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

// Command 保存解析后的命令，同时保留 rawLine，方便 write_file 等命令读取带空格的原始内容。
struct Command {
    std::string rawLine;
    std::string name;
    std::vector<std::string> arguments;

    [[nodiscard]] bool empty() const {
        return name.empty();
    }
};

// CommandRequest 由前台线程创建并放入 BlockingQueue，后台 worker 线程执行后通过 promise 回传响应。
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

// CommandContext 是 Dispatcher 只读上下文，用于 status/help 等命令展示当前内核状态。
struct CommandContext {
    std::uint64_t requestId = 0;
    std::string username;
    CommandSource source = CommandSource::LocalConsole;
    bool workerRunning = false;
    bool schedulerRunning = false;
    std::string schedulerOwner;
    std::uint32_t schedulerIntervalMs = 500;
    std::string snapshotPath;
    std::string autoLoadStatus;
};

} // namespace oscore
