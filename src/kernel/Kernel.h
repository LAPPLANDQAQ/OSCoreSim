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

// Kernel：模拟器中心协调器。
//
// 持有所有子系统实例（用户管理、进程管理、内存管理、调度器、快照存储、VFS、总览渲染器）。
// 负责启动/停止 worker 线程和自动调度线程，并协调命令执行和状态持久化。
//
// 架构：
//   - ConsoleApp（前台）→ submitCommand() → BlockingQueue → worker 线程（后台执行）
//   - scheduler 线程独立周期执行 MLFQ step，与命令线程共用 stateMutex_ 互斥
//   - NamedPipeServer 线程接收 Client 命令 → submitCommand() → 同一 BlockingQueue
//
// 线程安全：所有子系统状态修改必须通过 stateMutex_ 串行化。
class Kernel {
public:
    explicit Kernel(std::string snapshotPath = "data/os_state.bin");
    ~Kernel();

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // 启动内核：重置状态 → 自动加载快照 → 启动 worker 线程 + 调度线程
    void start();
    // 停止内核：设置关闭标志 → 关闭请求队列 → join 线程
    void stop();

    // 提交命令（阻塞等待结果）。Master 本地命令和 Pipe Client 远程命令都通过此接口。
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
    // worker 线程主循环：从 BlockingQueue 取请求 → executeRequest() → 通过 promise 回传结果
    void workerLoop();
    // 自动调度线程主循环：每 schedulerIntervalMs_ 毫秒执行一次 step()
    void schedulerLoop();

    // 命令执行核心：持锁 → 解析命令 → 路由到对应 handler
    [[nodiscard]] CommandResponse executeRequest(const CommandRequest& request);

    // 命令路由判断
    [[nodiscard]] bool isSchedulerCommand(const std::string& name) const;
    [[nodiscard]] bool isPersistenceCommand(const std::string& name) const;
    [[nodiscard]] bool isVisualizationCommand(const std::string& name) const;
    [[nodiscard]] bool isVfsCommand(const std::string& name) const;
    [[nodiscard]] bool isResetCommand(const std::string& name) const;

    // 各类命令处理器
    [[nodiscard]] CommandResponse handleSchedulerCommand(const Command& command, const CommandContext& context);
    [[nodiscard]] CommandResponse handlePersistenceCommand(const Command& command);
    [[nodiscard]] CommandResponse handleOverview(const Command& command);
    [[nodiscard]] CommandResponse handleVfsCommand(const Command& command);
    [[nodiscard]] CommandResponse handleResetCommand(const Command& command);

    // 快照导入/导出（在 stateMutex_ 保护下调用）
    [[nodiscard]] KernelSnapshot exportSnapshotLocked() const;
    bool importSnapshotLocked(const KernelSnapshot& snapshot, std::string& message, bool preserveCurrentSession);
    // 重置所有子系统到初始状态
    void resetStateLocked();
    // 验证快照数据的一致性（用户、PCB、内存块的交叉检查）
    [[nodiscard]] bool validateSnapshot(const KernelSnapshot& snapshot, std::string& message) const;
    [[nodiscard]] std::string snapshotSummaryText(const KernelSnapshot& snapshot) const;

    // 主状态锁：保护 ProcessManager、MemoryManager、UserManager、VFS 之间的组合操作。
    // 所有 executeRequest() 调用都在持锁下进行。
    mutable std::mutex stateMutex_;
    std::mutex consoleMutex_;  // 保护 std::cout 输出（防止自动调度日志与命令输出交错）

    BlockingQueue<CommandRequest> requestQueue_;  // 命令请求队列

    std::thread workerThread_;      // 命令执行线程
    std::thread schedulerThread_;   // 自动调度线程

    // 子系统实例（按功能模块组织）
    CommandDispatcher dispatcher_;
    UserManager userManager_;
    ProcessManager processManager_;
    MemoryManager memoryManager_;
    Scheduler scheduler_;
    SnapshotStore snapshotStore_;
    OverviewRenderer overviewRenderer_;
    VirtualFileSystem vfs_;

    std::uint64_t nextRequestId_ = 1;             // 请求编号计数器

    std::atomic<bool> schedulerRunning_{false};   // 自动调度运行标志（原子变量，避免锁争用）
    std::atomic<bool> shuttingDown_{false};       // 关闭标志
    std::string schedulerOwner_;                  // 调度器所属用户
    std::uint32_t schedulerIntervalMs_ = 500;     // 自动调度间隔（毫秒）
    std::string startupMessage_;                  // 启动信息（快照加载状态等）
    std::string autoLoadStatus_ = "not checked";  // 自动加载状态描述

    bool started_ = false;       // 内核是否已启动
    bool stopping_ = false;      // 是否正在停止中
    bool workerRunning_ = false; // worker 线程是否运行中
};

} // namespace oscore
