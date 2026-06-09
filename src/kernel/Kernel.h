#pragma once

#include "auth/UserManager.h"
#include "kernel/CommandDispatcher.h"
#include "kernel/CommandTypes.h"
#include "memory/MemoryManager.h"
#include "persistence/SnapshotStore.h"
#include "process/ProcessManager.h"
#include "process/Scheduler.h"
#include "util/BlockingQueue.h"
#include "view/OverviewRenderer.h"
#include "vfs/VirtualFileSystem.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace oscore {

// Kernel 是模拟器的中心协调者：拥有用户、进程、内存、调度器、VFS 和快照存储。
// ConsoleApp/NamedPipe 只能提交命令，所有共享状态修改必须通过 Kernel worker 线程和 stateMutex_ 串行协调。
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
    [[nodiscard]] bool isVisualizationCommand(const std::string& name) const;
    [[nodiscard]] bool isVfsCommand(const std::string& name) const;
    [[nodiscard]] CommandResponse handleSchedulerCommand(const Command& command, const CommandContext& context);
    [[nodiscard]] CommandResponse handlePersistenceCommand(const Command& command);
    [[nodiscard]] CommandResponse handleOverview(const Command& command);
    [[nodiscard]] CommandResponse handleVfsCommand(const Command& command);
    [[nodiscard]] KernelSnapshot exportSnapshotLocked() const;
    bool importSnapshotLocked(const KernelSnapshot& snapshot, std::string& message, bool preserveCurrentSession);
    void resetStateLocked();
    [[nodiscard]] bool validateSnapshot(const KernelSnapshot& snapshot, std::string& message) const;
    [[nodiscard]] std::string snapshotSummaryText(const KernelSnapshot& snapshot) const;

    // stateMutex_ 保护 ProcessManager、MemoryManager、UserManager、VFS 之间的组合操作。
    mutable std::mutex stateMutex_;
    std::mutex consoleMutex_;
    // 前台只入队，workerThread_ 负责真正执行命令，满足前后台线程分离要求。
    BlockingQueue<CommandRequest> requestQueue_;
    std::thread workerThread_;
    // schedulerThread_ 独立周期执行 MLFQ step，和命令线程共用 stateMutex_ 避免竞态。
    std::thread schedulerThread_;
    CommandDispatcher dispatcher_;
    UserManager userManager_;
    ProcessManager processManager_;
    MemoryManager memoryManager_;
    Scheduler scheduler_;
    SnapshotStore snapshotStore_;
    OverviewRenderer overviewRenderer_;
    VirtualFileSystem vfs_;
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
