#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "kernel/CommandTypes.h"
#include "memory/MemoryManager.h"
#include "persistence/SnapshotStore.h"
#include "process/ProcessManager.h"
#include "process/Scheduler.h"
#include "util/BlockingQueue.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace oscore {

class Kernel {
public:
    explicit Kernel(std::string snapshotPath = "data/os_state.bin");
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
    [[nodiscard]] std::string startupMessage() const;
    [[nodiscard]] KernelSnapshot exportSnapshot() const;
    bool importSnapshot(const KernelSnapshot& snapshot, std::string& message);

private:
    void workerLoop();
    void schedulerLoop();
    [[nodiscard]] CommandResponse executeRequest(const CommandRequest& request);
    [[nodiscard]] bool isSchedulerCommand(const std::string& name) const;
    [[nodiscard]] bool isPersistenceCommand(const std::string& name) const;
    [[nodiscard]] CommandResponse handleSchedulerCommand(const Command& command, const CommandContext& context);
    [[nodiscard]] CommandResponse handlePersistenceCommand(const Command& command);
    [[nodiscard]] KernelSnapshot exportSnapshotLocked() const;
    bool importSnapshotLocked(const KernelSnapshot& snapshot, std::string& message);
    void resetStateLocked();
    [[nodiscard]] bool validateSnapshot(const KernelSnapshot& snapshot, std::string& message) const;
    [[nodiscard]] std::string snapshotSummaryText(const KernelSnapshot& snapshot) const;

    mutable std::mutex stateMutex_;
    std::mutex consoleMutex_;
    BlockingQueue<CommandRequest> requestQueue_;
    std::thread workerThread_;
    std::thread schedulerThread_;
    CommandDispatcher dispatcher_;
    UserManager userManager_;
    ProcessManager processManager_;
    MemoryManager memoryManager_;
    Scheduler scheduler_;
    SnapshotStore snapshotStore_;
    std::uint64_t nextRequestId_ = 1;
    std::atomic<bool> schedulerRunning_{false};
    std::atomic<bool> shuttingDown_{false};
    std::string schedulerOwner_;
    std::uint32_t schedulerIntervalMs_ = 500;
    std::string startupMessage_;
    std::string autoLoadStatus_ = "not checked";
    bool started_ = false;
    bool stopping_ = false;
    bool workerRunning_ = false;
};

} // namespace oscore
