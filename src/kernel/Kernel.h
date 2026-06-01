#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "kernel/CommandTypes.h"
#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"
#include "util/BlockingQueue.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace oscore {

class Kernel {
public:
    Kernel() = default;
    ~Kernel();

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    void start();
    void stop();

    [[nodiscard]] CommandResponse submitCommand(
        const std::string& rawLine,
        const std::string& username = "",
        CommandSource source = CommandSource::LocalConsole);

    [[nodiscard]] bool isWorkerRunning() const;
    [[nodiscard]] bool isLoggedIn() const;
    [[nodiscard]] std::string currentUser() const;

private:
    void workerLoop();
    [[nodiscard]] CommandResponse executeRequest(const CommandRequest& request);

    mutable std::mutex stateMutex_;
    BlockingQueue<CommandRequest> requestQueue_;
    std::thread workerThread_;
    CommandDispatcher dispatcher_;
    UserManager userManager_;
    ProcessManager processManager_;
    MemoryManager memoryManager_;
    std::uint64_t nextRequestId_ = 1;
    bool started_ = false;
    bool stopping_ = false;
    bool workerRunning_ = false;
};

} // namespace oscore
