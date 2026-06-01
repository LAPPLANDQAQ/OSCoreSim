#include "kernel/Kernel.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oscore {

Kernel::Kernel(std::string snapshotPath) : snapshotStore_(std::move(snapshotPath)) {}

Kernel::~Kernel() {
    stop();
}

void Kernel::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (started_) {
        return;
    }

    // 每次启动都先恢复到干净内核，再按需导入二进制快照，避免上一次运行的内存态残留。
    requestQueue_.reset();
    stopping_ = false;
    shuttingDown_.store(false);
    schedulerRunning_.store(false);
    scheduler_.setRunning(false);
    schedulerOwner_.clear();
    resetStateLocked();

    if (snapshotStore_.exists()) {
        KernelSnapshot snapshot;
        std::string loadMessage;
        if (snapshotStore_.load(snapshot, loadMessage) && importSnapshotLocked(snapshot, loadMessage)) {
            const auto summary = snapshotStore_.summarize(snapshot);
            std::ostringstream boot;
            boot << "[BOOT] Found " << snapshotStore_.defaultPath() << ", loading previous system state...\n"
                 << "[BOOT] Load success. Users=" << summary.users
                 << ", Processes=" << summary.processes
                 << ", MemoryBlocks=" << summary.memoryBlocks << ".\n"
                 << "[BOOT] Please login to continue.";
            startupMessage_ = boot.str();
            autoLoadStatus_ = "success";
        } else {
            resetStateLocked();
            startupMessage_ = "[BOOT] State file exists but failed to load: " + loadMessage +
                "\n[BOOT] Starting with a clean system.\n[WARN] The corrupted state file was not overwritten automatically.";
            autoLoadStatus_ = "failed: " + loadMessage;
        }
    } else {
        startupMessage_ = "[BOOT] No state file found. Starting with a clean system.";
        autoLoadStatus_ = "no state file";
    }

    workerRunning_ = false;
    started_ = true;
    workerThread_ = std::thread(&Kernel::workerLoop, this);
    schedulerThread_ = std::thread(&Kernel::schedulerLoop, this);
}

void Kernel::stop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!started_ && !workerThread_.joinable() && !schedulerThread_.joinable()) {
            return;
        }
        stopping_ = true;
        schedulerRunning_.store(false);
        scheduler_.setRunning(false);
        schedulerOwner_.clear();
        shuttingDown_.store(true);
    }

    requestQueue_.shutdown();

    if (schedulerThread_.joinable()) {
        schedulerThread_.join();
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    started_ = false;
    workerRunning_ = false;
}

CommandResponse Kernel::submitCommand(
    const std::string& rawLine,
    const std::string& username,
    CommandSource source) {
    auto promise = std::make_shared<std::promise<CommandResponse>>();
    auto future = promise->get_future();

    CommandRequest request;
    request.rawLine = rawLine;
    request.username = username;
    request.source = source;
    request.promise = std::move(promise);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!started_ || stopping_) {
            return {false, "Kernel is not running.", false};
        }
        request.id = nextRequestId_++;
    }

    try {
        requestQueue_.push(std::move(request));
    } catch (const std::exception& ex) {
        std::ostringstream output;
        output << "Failed to submit command: " << ex.what();
        return {false, output.str(), false};
    }

    return future.get();
}

bool Kernel::isWorkerRunning() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return workerRunning_;
}

bool Kernel::isLoggedIn() const {
    return userManager_.isLoggedIn();
}

std::string Kernel::currentUser() const {
    return userManager_.currentUser();
}

std::string Kernel::startupMessage() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return startupMessage_;
}

KernelSnapshot Kernel::exportSnapshot() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return exportSnapshotLocked();
}

bool Kernel::importSnapshot(const KernelSnapshot& snapshot, std::string& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return importSnapshotLocked(snapshot, message);
}

void Kernel::workerLoop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        workerRunning_ = true;
    }

    CommandRequest request;
    while (requestQueue_.pop(request)) {
        auto response = executeRequest(request);
        if (request.promise) {
            request.promise->set_value(response);
        }

        if (response.shouldExit) {
            // exit/quit 由后台线程确认后触发队列关闭，前台随后调用 stop() 完成 join。
            requestQueue_.shutdown();
            break;
        }
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    workerRunning_ = false;
}

void Kernel::schedulerLoop() {
    while (!shuttingDown_.load()) {
        if (!schedulerRunning_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string log;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (schedulerRunning_.load() && !schedulerOwner_.empty()) {
                // 自动调度线程只在 Kernel 的统一状态锁内修改 PCB 和内存，避免命令线程并发改同一份状态。
                log = scheduler_.step(schedulerOwner_, processManager_, memoryManager_);
            }
        }

        if (!log.empty()) {
            std::lock_guard<std::mutex> consoleLock(consoleMutex_);
            std::cout << "\n[SCHED AUTO]\n" << log << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(schedulerIntervalMs_));
    }
}

CommandResponse Kernel::executeRequest(const CommandRequest& request) {
    try {
        const auto command = dispatcher_.parse(request.rawLine);
        std::lock_guard<std::mutex> lock(stateMutex_);
        const CommandContext context{
            request.id,
            userManager_.currentUser(),
            request.source,
            workerRunning_,
            schedulerRunning_.load(),
            schedulerOwner_,
            schedulerIntervalMs_,
            snapshotStore_.defaultPath(),
            autoLoadStatus_
        };

        CommandResponse response;
        if (isSchedulerCommand(command.name)) {
            response = handleSchedulerCommand(command, context);
        } else if (isPersistenceCommand(command.name)) {
            response = handlePersistenceCommand(command);
        } else if (isVisualizationCommand(command.name)) {
            response = handleOverview(command);
        } else {
            response = dispatcher_.dispatch(command, context, userManager_, processManager_, memoryManager_);
        }

        if (response.shouldExit || (response.success && command.name == "logout")) {
            schedulerRunning_.store(false);
            scheduler_.setRunning(false);
            schedulerOwner_.clear();
        }
        return response;
    } catch (const std::exception& ex) {
        std::ostringstream output;
        output << "Command execution failed: " << ex.what();
        return {false, output.str(), false};
    }
}

bool Kernel::isSchedulerCommand(const std::string& name) const {
    return name == "start_sched" ||
           name == "stop_sched" ||
           name == "restart_sched" ||
           name == "step";
}

bool Kernel::isPersistenceCommand(const std::string& name) const {
    return name == "save" || name == "load";
}

bool Kernel::isVisualizationCommand(const std::string& name) const {
    return name == "overview";
}

CommandResponse Kernel::handleSchedulerCommand(const Command& command, const CommandContext& context) {
    (void)context;
    if (!userManager_.isLoggedIn()) {
        return {false, "This command requires login. Please run: login <username> <password>", false};
    }

    const auto owner = userManager_.currentUser();
    if (command.name == "step") {
        if (!command.arguments.empty()) {
            return {false, "Usage: step", false};
        }
        return {true, scheduler_.step(owner, processManager_, memoryManager_), false};
    }

    if (command.name == "start_sched") {
        if (!command.arguments.empty()) {
            return {false, "Usage: start_sched", false};
        }
        if (schedulerRunning_.load()) {
            return {true, "[INFO] MLFQ scheduler is already running for user " + schedulerOwner_ + ".", false};
        }
        schedulerOwner_ = owner;
        schedulerRunning_.store(true);
        scheduler_.setRunning(true);

        std::ostringstream output;
        output << "[OK] MLFQ scheduler started for user " << schedulerOwner_ << ".\n"
               << "Auto scheduling interval: " << schedulerIntervalMs_ << "ms.";
        return {true, output.str(), false};
    }

    if (command.name == "stop_sched") {
        if (!command.arguments.empty()) {
            return {false, "Usage: stop_sched", false};
        }
        if (!schedulerRunning_.load()) {
            return {true, "[INFO] Scheduler is not running.", false};
        }
        schedulerRunning_.store(false);
        scheduler_.setRunning(false);

        std::ostringstream output;
        output << "[OK] Scheduler stopped.\n"
               << "Current Ready Queues:\n"
               << processManager_.readyQueueSnapshot(owner);
        return {true, output.str(), false};
    }

    if (command.name == "restart_sched") {
        if (!command.arguments.empty()) {
            return {false, "Usage: restart_sched", false};
        }
        schedulerRunning_.store(false);
        scheduler_.setRunning(false);
        const auto removed = processManager_.cleanupInvalidReadyQueueEntries(owner);
        schedulerOwner_ = owner;
        schedulerRunning_.store(true);
        scheduler_.setRunning(true);

        std::ostringstream output;
        output << "[OK] Scheduler restarted.\n"
               << "Invalid ready queue entries removed: " << removed.size() << '\n'
               << "Scheduler owner: " << schedulerOwner_;
        return {true, output.str(), false};
    }

    return {false, "Unknown scheduler command.", false};
}

CommandResponse Kernel::handlePersistenceCommand(const Command& command) {
    if (!command.arguments.empty()) {
        return {false, "Usage: " + command.name, false};
    }

    if (command.name == "save") {
        const auto snapshot = exportSnapshotLocked();
        std::string message;
        if (!snapshotStore_.save(snapshot, message)) {
            return {false, message, false};
        }

        std::ostringstream output;
        output << "[OK] System state saved to " << snapshotStore_.defaultPath() << '\n'
               << snapshotSummaryText(snapshot);
        return {true, output.str(), false};
    }

    if (command.name == "load") {
        schedulerRunning_.store(false);
        scheduler_.setRunning(false);
        schedulerOwner_.clear();

        KernelSnapshot snapshot;
        std::string message;
        if (!snapshotStore_.load(snapshot, message)) {
            return {false, message, false};
        }
        if (!importSnapshotLocked(snapshot, message)) {
            return {false, "Load failed: " + message, false};
        }

        std::ostringstream output;
        output << "[OK] System state loaded from " << snapshotStore_.defaultPath() << '\n'
               << snapshotSummaryText(snapshot) << '\n'
               << "Scheduler: STOPPED after load\n"
               << "Please login again.";
        return {true, output.str(), false};
    }

    return {false, "Unknown persistence command.", false};
}

CommandResponse Kernel::handleOverview(const Command& command) {
    // overview 是只读可视化命令，必须要求用户登录
    if (!userManager_.isLoggedIn()) {
        return {false, "[ERROR] Please login before using overview.", false};
    }

    if (!command.arguments.empty()) {
        return {false, "Usage: overview", false};
    }

    // stateMutex_ 已在 executeRequest() 中锁定，此时处于锁保护期间
    const auto currentUser = userManager_.currentUser();

    // 获取 ProcessManager 和 MemoryManager 的只读快照
    const auto userProcesses = processManager_.getProcessCopiesForUser(currentUser);
    const auto readyQueues = processManager_.exportReadyQueues();
    const auto memoryBlocks = memoryManager_.exportBlocks();
    const auto totalMemKB = memoryManager_.totalMemoryKB();

    // 构建调度器信息
    OverviewRenderer::SchedulerInfo schedulerInfo;
    schedulerInfo.running = schedulerRunning_.load();
    schedulerInfo.owner = schedulerOwner_;
    schedulerInfo.intervalMs = static_cast<int>(schedulerIntervalMs_);

    // 渲染 overview 输出
    const auto output = overviewRenderer_.render(
        currentUser,
        userProcesses,
        readyQueues,
        memoryBlocks,
        totalMemKB,
        schedulerInfo,
        snapshotStore_.defaultPath(),
        memoryManager_.currentAlgorithmName());

    return {true, output, false};
}

KernelSnapshot Kernel::exportSnapshotLocked() const {
    KernelSnapshot snapshot;
    snapshot.users = userManager_.exportUsers();
    snapshot.nextPid = processManager_.exportNextPid();
    snapshot.pcbs = processManager_.exportPcbs();
    snapshot.readyQueues = processManager_.exportReadyQueues();
    snapshot.memoryBlocks = memoryManager_.exportBlocks();
    snapshot.totalMemoryKB = memoryManager_.totalMemoryKB();
    snapshot.allocAlgorithm = memoryManager_.currentAlgorithm();
    snapshot.schedulerRunning = schedulerRunning_.load();
    snapshot.schedulerOwner = schedulerOwner_;
    return snapshot;
}

bool Kernel::importSnapshotLocked(const KernelSnapshot& snapshot, std::string& message) {
    if (!validateSnapshot(snapshot, message)) {
        return false;
    }

    // 载入快照时不恢复交互会话和自动调度，避免重启后绕过登录或后台线程立即修改状态。
    schedulerRunning_.store(false);
    scheduler_.setRunning(false);
    schedulerOwner_.clear();

    userManager_.importUsers(snapshot.users);
    userManager_.clearCurrentSession();

    processManager_.importPcbs(snapshot.pcbs);
    processManager_.importNextPid(snapshot.nextPid);
    processManager_.importReadyQueues(snapshot.readyQueues);
    processManager_.rebuildParentChildRelationsIfNeeded();
    std::string queueMessage;
    processManager_.validateReadyQueues(queueMessage);

    memoryManager_.setTotalMemoryKB(snapshot.totalMemoryKB);
    memoryManager_.setAlgorithmDirect(snapshot.allocAlgorithm);
    memoryManager_.importBlocks(snapshot.memoryBlocks);
    std::string memoryMessage;
    if (!memoryManager_.validateBlocks(memoryMessage)) {
        message = memoryMessage;
        return false;
    }

    autoLoadStatus_ = "manual load success";
    message = queueMessage;
    return true;
}

void Kernel::resetStateLocked() {
    userManager_.importUsers({});
    userManager_.clearCurrentSession();
    processManager_.importPcbs({});
    processManager_.importNextPid(1);
    processManager_.importReadyQueues({});
    memoryManager_.setTotalMemoryKB(1024);
    memoryManager_.setAlgorithmDirect(AllocAlgorithm::FIRST_FIT);
    memoryManager_.importBlocks({MemoryBlock{0, 1024, MemoryBlockType::FREE, 0, "", ""}});
    schedulerRunning_.store(false);
    scheduler_.setRunning(false);
    schedulerOwner_.clear();
}

bool Kernel::validateSnapshot(const KernelSnapshot& snapshot, std::string& message) const {
    std::unordered_set<std::string> usernames;
    for (const auto& account : snapshot.users) {
        if (account.username.empty()) {
            message = "invalid snapshot: empty username.";
            return false;
        }
        if (!usernames.insert(account.username).second) {
            message = "invalid snapshot: duplicated username.";
            return false;
        }
    }

    std::unordered_map<std::uint32_t, PCB> pcbs;
    std::uint32_t maxPid = 0;
    for (const auto& pcb : snapshot.pcbs) {
        if (pcb.pid == 0 || !pcbs.emplace(pcb.pid, pcb).second) {
            message = "invalid snapshot: duplicated or zero PID.";
            return false;
        }
        if (usernames.find(pcb.owner) == usernames.end()) {
            message = "invalid snapshot: PCB owner does not exist.";
            return false;
        }
        if (pcb.ppid == pcb.pid) {
            message = "invalid snapshot: PCB cannot be its own parent.";
            return false;
        }
        if (pcb.priority < 0 || pcb.priority > 15 || pcb.queueLevel < 0 || pcb.queueLevel > 2) {
            message = "invalid snapshot: PCB priority or queue level out of range.";
            return false;
        }
        if (pcb.executedTime > pcb.totalTime || pcb.remainingTime > pcb.totalTime) {
            message = "invalid snapshot: PCB runtime counters out of range.";
            return false;
        }
        maxPid = std::max(maxPid, pcb.pid);
    }

    if (snapshot.nextPid <= maxPid) {
        message = "invalid snapshot: nextPid must be greater than existing PIDs.";
        return false;
    }

    for (const auto& pcb : snapshot.pcbs) {
        if (pcb.ppid != 0 && pcbs.find(pcb.ppid) == pcbs.end()) {
            message = "invalid snapshot: parent PID does not exist.";
            return false;
        }
        for (const auto child : pcb.children) {
            if (pcbs.find(child) == pcbs.end()) {
                message = "invalid snapshot: child PID does not exist.";
                return false;
            }
        }
    }

    if (snapshot.totalMemoryKB == 0) {
        message = "invalid snapshot: total memory must be greater than 0.";
        return false;
    }

    for (const auto& block : snapshot.memoryBlocks) {
        if (block.type == MemoryBlockType::PROCESS) {
            auto process = pcbs.find(block.pid);
            if (process == pcbs.end() || process->second.owner != block.owner) {
                message = "invalid snapshot: PROCESS memory block does not match a PCB.";
                return false;
            }
        }
        if ((block.type == MemoryBlockType::KERNEL || block.type == MemoryBlockType::PROCESS) &&
            usernames.find(block.owner) == usernames.end()) {
            message = "invalid snapshot: memory owner does not exist.";
            return false;
        }
    }

    MemoryManager memoryValidator;
    memoryValidator.setTotalMemoryKB(snapshot.totalMemoryKB);
    memoryValidator.setAlgorithmDirect(snapshot.allocAlgorithm);
    memoryValidator.importBlocks(snapshot.memoryBlocks);
    return memoryValidator.validateBlocks(message);
}

std::string Kernel::snapshotSummaryText(const KernelSnapshot& snapshot) const {
    const auto summary = snapshotStore_.summarize(snapshot);
    std::ostringstream output;
    output << "Users: " << summary.users << '\n'
           << "Processes: " << summary.processes << '\n'
           << "Memory Blocks: " << summary.memoryBlocks << '\n'
           << "Ready Queues: Q0=" << summary.readyQueueSizes[0]
           << ", Q1=" << summary.readyQueueSizes[1]
           << ", Q2=" << summary.readyQueueSizes[2];
    return output.str();
}

} // namespace oscore
