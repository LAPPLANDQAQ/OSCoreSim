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
namespace {

std::string extractWriteFileContent(const Command& command) {
    const auto& line = command.rawLine;
    auto pos = line.find_first_not_of(" \t\r\n");
    if (pos == std::string::npos) { return {}; }
    pos = line.find_first_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    pos = line.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    pos = line.find_first_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    pos = line.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    return line.substr(pos);
}

std::string decodeEscapeSequences(const std::string& raw) {
    if (raw.empty()) return raw;
    std::string result;
    result.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            switch (raw[i + 1]) {
            case 'n': result.push_back('\n'); ++i; break;
            case 'r': result.push_back('\r'); ++i; break;
            case 't': result.push_back('\t'); ++i; break;
            case '\\': result.push_back('\\'); ++i; break;
            case '"': result.push_back('"'); ++i; break;
            default: result.push_back('\\'); break;
            }
        } else {
            result.push_back(raw[i]);
        }
    }
    return result;
}

} // namespace

Kernel::Kernel(std::string snapshotPath) : snapshotStore_(std::move(snapshotPath)) {}

Kernel::~Kernel() { stop(); }

// start()：内核启动流程。
// 1. 重置请求队列和关闭标志
// 2. 恢复所有子系统到初始干净状态
// 3. 检测快照文件是否存在：
//    a) 存在 → 加载并导入快照，若成功则设置"加载成功"消息，若失败则以干净状态启动
//    b) 不存在 → 以干净系统启动
// 4. 启动 worker 线程（处理命令队列）和 scheduler 线程（自动调度）
void Kernel::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (started_) return;

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
        if (snapshotStore_.load(snapshot, loadMessage) && importSnapshotLocked(snapshot, loadMessage, false)) {
            const auto summary = snapshotStore_.summarize(snapshot);
            std::ostringstream boot;
            boot << "发现快照文件 " << snapshotStore_.defaultPath()
                 << "，正在加载上次系统状态...\n"
                 << "加载成功。用户=" << summary.users
                 << "，进程=" << summary.processes
                 << "，内存块=" << summary.memoryBlocks << "。\n"
                 << "请登录以继续。";
            startupMessage_ = boot.str();
            autoLoadStatus_ = "加载成功";
        } else {
            resetStateLocked();
            startupMessage_ = "快照文件存在但加载失败：" + loadMessage +
                "\n以干净系统启动。损坏的快照文件未被自动覆盖。";
            autoLoadStatus_ = "加载失败: " + loadMessage;
        }
    } else {
        startupMessage_ = "未找到快照文件，以干净系统启动。";
        autoLoadStatus_ = "无快照文件";
    }

    workerRunning_ = false;
    started_ = true;
    workerThread_ = std::thread(&Kernel::workerLoop, this);
    schedulerThread_ = std::thread(&Kernel::schedulerLoop, this);
}

void Kernel::stop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!started_ && !workerThread_.joinable() && !schedulerThread_.joinable()) return;
        stopping_ = true;
        schedulerRunning_.store(false);
        scheduler_.setRunning(false);
        schedulerOwner_.clear();
        shuttingDown_.store(true);
    }
    requestQueue_.shutdown();
    if (schedulerThread_.joinable()) schedulerThread_.join();
    if (workerThread_.joinable()) workerThread_.join();
    std::lock_guard<std::mutex> lock(stateMutex_);
    started_ = false; workerRunning_ = false;
}

CommandResponse Kernel::submitCommand(const std::string& rawLine, const std::string& username, CommandSource source) {
    auto promise = std::make_shared<std::promise<CommandResponse>>();
    auto future = promise->get_future();
    CommandRequest request;
    request.rawLine = rawLine; request.username = username; request.source = source;
    request.promise = std::move(promise);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!started_ || stopping_) return {false, "[错误] 内核未运行。", false};
        request.id = nextRequestId_++;
    }
    try { requestQueue_.push(std::move(request)); }
    catch (const std::exception& ex) {
        std::ostringstream output;
        output << "[错误] 提交命令失败: " << ex.what();
        return {false, output.str(), false};
    }
    return future.get();
}

bool Kernel::isWorkerRunning() const { std::lock_guard<std::mutex> lock(stateMutex_); return workerRunning_; }
bool Kernel::isLoggedIn() const { return userManager_.isLoggedIn(); }
std::string Kernel::currentUser() const { return userManager_.currentUser(); }
std::string Kernel::startupMessage() const { std::lock_guard<std::mutex> lock(stateMutex_); return startupMessage_; }
KernelSnapshot Kernel::exportSnapshot() const { std::lock_guard<std::mutex> lock(stateMutex_); return exportSnapshotLocked(); }
bool Kernel::importSnapshot(const KernelSnapshot& snapshot, std::string& message) { std::lock_guard<std::mutex> lock(stateMutex_); return importSnapshotLocked(snapshot, message, false); }

// workerLoop：命令执行线程主循环。
// 从 BlockingQueue 阻塞获取命令 → executeRequest() 串行执行 → 通过 promise 回传结果。
// 收到 shouldExit 响应后关闭队列并退出循环。
// 所有命令通过 stateMutex_ 串行执行，避免并发修改内核状态。
void Kernel::workerLoop() {
    { std::lock_guard<std::mutex> lock(stateMutex_); workerRunning_ = true; }
    CommandRequest request;
    while (requestQueue_.pop(request)) {
        auto response = executeRequest(request);
        if (request.promise) request.promise->set_value(response);
        if (response.shouldExit) { requestQueue_.shutdown(); break; }
    }
    std::lock_guard<std::mutex> lock(stateMutex_); workerRunning_ = false;
}

// schedulerLoop：自动调度线程主循环。
// 每 schedulerIntervalMs_（默认 500ms）检查 schedulerRunning 标志：
//   - 如果为 true → 持锁执行一次 Scheduler::step() → 打印调度日志
//   - 如果为 false → 休眠 100ms 后重试
// step() 内部会修改 PCB 状态、就绪队列和内存块，与命令线程通过 stateMutex_ 互斥。
void Kernel::schedulerLoop() {
    while (!shuttingDown_.load()) {
        if (!schedulerRunning_.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        std::string log;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (schedulerRunning_.load() && !schedulerOwner_.empty())
                log = scheduler_.step(schedulerOwner_, processManager_, memoryManager_);
        }
        if (!log.empty()) { std::lock_guard<std::mutex> consoleLock(consoleMutex_); std::cout << "\n[自动调度]\n" << log << std::endl; }
        std::this_thread::sleep_for(std::chrono::milliseconds(schedulerIntervalMs_));
    }
}

CommandResponse Kernel::executeRequest(const CommandRequest& request) {
    try {
        const auto command = dispatcher_.parse(request.rawLine);
        std::lock_guard<std::mutex> lock(stateMutex_);
        const CommandContext context{request.id, userManager_.currentUser(), request.source, workerRunning_,
            schedulerRunning_.load(), schedulerOwner_, schedulerIntervalMs_,
            snapshotStore_.defaultPath(), autoLoadStatus_};
        CommandResponse response;
        if (isSchedulerCommand(command.name)) response = handleSchedulerCommand(command, context);
        else if (isPersistenceCommand(command.name)) response = handlePersistenceCommand(command);
        else if (isVisualizationCommand(command.name)) response = handleOverview(command);
        else if (isVfsCommand(command.name)) response = handleVfsCommand(command);
        else if (isResetCommand(command.name)) response = handleResetCommand(command);
        else response = dispatcher_.dispatch(command, context, userManager_, processManager_, memoryManager_);
        if (response.shouldExit || (response.success && command.name == "logout")) {
            schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
        }
        return response;
    } catch (const std::exception& ex) {
        std::ostringstream output;
        output << "[错误] 命令执行失败: " << ex.what();
        return {false, output.str(), false};
    }
}

bool Kernel::isSchedulerCommand(const std::string& name) const { return name == "start_sched" || name == "start" || name == "stop_sched" || name == "stop" || name == "restart_sched" || name == "restart" || name == "step"; }
bool Kernel::isPersistenceCommand(const std::string& name) const { return name == "save" || name == "load"; }
bool Kernel::isVisualizationCommand(const std::string& name) const { return name == "overview"; }

CommandResponse Kernel::handleSchedulerCommand(const Command& command, const CommandContext& context) {
    (void)context;
    if (!userManager_.isLoggedIn()) return {false, "[提示] 当前命令需要先登录。用法：login <用户名> <密码>", false};
    const auto owner = userManager_.currentUser();
    std::string schedulerCommand = command.name;
    if (schedulerCommand == "start") schedulerCommand = "start_sched";
    else if (schedulerCommand == "stop") schedulerCommand = "stop_sched";
    else if (schedulerCommand == "restart") schedulerCommand = "restart_sched";

    if (schedulerCommand == "step") {
        if (!command.arguments.empty()) return {false, "用法：step", false};
        return {true, scheduler_.step(owner, processManager_, memoryManager_), false};
    }
    if (schedulerCommand == "start_sched") {
        if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
        if (schedulerRunning_.load()) return {true, "[提示] MLFQ 调度器已在为用户 " + schedulerOwner_ + " 运行。", false};
        schedulerOwner_ = owner; schedulerRunning_.store(true); scheduler_.setRunning(true);
        std::ostringstream output;
        output << "[成功] MLFQ 调度器已为用户 " << schedulerOwner_ << " 启动。\n自动调度间隔: " << schedulerIntervalMs_ << "ms。";
        return {true, output.str(), false};
    }
    if (schedulerCommand == "stop_sched") {
        if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
        if (!schedulerRunning_.load()) return {true, "[提示] 调度器未运行。", false};
        schedulerRunning_.store(false); scheduler_.setRunning(false);
        std::ostringstream output;
        output << "[成功] 调度器已停止。\n当前就绪队列:\n" << processManager_.readyQueueSnapshot(owner);
        return {true, output.str(), false};
    }
    if (schedulerCommand == "restart_sched") {
        if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
        schedulerRunning_.store(false); scheduler_.setRunning(false);
        const auto removed = processManager_.cleanupInvalidReadyQueueEntries(owner);
        schedulerOwner_ = owner; schedulerRunning_.store(true); scheduler_.setRunning(true);
        std::ostringstream output;
        output << "[成功] 调度器已重启。\n已移除无效就绪队列条目: " << removed.size() << "\n调度用户: " << schedulerOwner_;
        return {true, output.str(), false};
    }
    return {false, "[错误] 未知调度命令。", false};
}

CommandResponse Kernel::handlePersistenceCommand(const Command& command) {
    if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
    if (command.name == "save") {
        const auto snapshot = exportSnapshotLocked();
        std::string message;
        if (!snapshotStore_.save(snapshot, message)) return {false, message, false};
        std::ostringstream output;
        output << "[成功] 系统状态已保存到 " << snapshotStore_.defaultPath() << '\n' << snapshotSummaryText(snapshot);
        return {true, output.str(), false};
    }
    if (command.name == "load") {
        schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
        const auto previousUser = userManager_.currentUser();
        KernelSnapshot snapshot; std::string message;
        if (!snapshotStore_.load(snapshot, message)) return {false, message, false};
        if (!importSnapshotLocked(snapshot, message, true)) return {false, "加载失败: " + message, false};
        const bool sessionPreserved = !previousUser.empty() && userManager_.currentUser() == previousUser;
        std::ostringstream output;
        output << "[成功] 系统状态已从 " << snapshotStore_.defaultPath() << " 加载。\n" << snapshotSummaryText(snapshot) << "\n调度器: 加载后已停止\n";
        if (sessionPreserved) output << "会话: 已保留用户 " << previousUser << " 的登录。";
        else output << "会话: 加载后已清除，请重新登录。";
        return {true, output.str(), false};
    }
    return {false, "[错误] 未知持久化命令。", false};
}

CommandResponse Kernel::handleOverview(const Command& command) {
    if (!userManager_.isLoggedIn()) return {false, "[错误] 请先登录再使用 overview。", false};
    if (!command.arguments.empty()) return {false, "用法：overview", false};
    const auto currentUser = userManager_.currentUser();
    const auto userProcesses = processManager_.getProcessCopiesForUser(currentUser);
    const auto readyQueues = processManager_.exportReadyQueues();
    const auto memoryBlocks = memoryManager_.exportBlocks();
    const auto totalMemKB = memoryManager_.totalMemoryKB();
    const auto vfsFileCount = vfs_.fileCountForUser(currentUser);
    OverviewRenderer::SchedulerInfo schedulerInfo;
    schedulerInfo.running = schedulerRunning_.load(); schedulerInfo.owner = schedulerOwner_;
    schedulerInfo.intervalMs = static_cast<int>(schedulerIntervalMs_);
    const auto output = overviewRenderer_.render(currentUser, userProcesses, readyQueues, memoryBlocks, totalMemKB, schedulerInfo, snapshotStore_.defaultPath(), memoryManager_.currentAlgorithmName(), vfsFileCount);
    return {true, output, false};
}

CommandResponse Kernel::handleResetCommand(const Command& command) {
    if (!command.arguments.empty()) return {false, "用法：reset_system", false};
    schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
    resetStateLocked();
    return {true, "[成功] 系统已重置到干净状态。", false};
}

CommandResponse Kernel::handleVfsCommand(const Command& command) {
    if (!userManager_.isLoggedIn()) return {false, "[提示] 当前命令需要先登录。用法：login <用户名> <密码>", false};
    const auto owner = userManager_.currentUser();
    if (command.name == "touch_file") {
        if (command.arguments.size() != 1) return {false, "用法：touch_file <文件名>", false};
        std::string message; const bool ok = vfs_.createFile(owner, command.arguments[0], message);
        return {ok, message, false};
    }
    if (command.name == "write_file") {
        if (command.arguments.size() < 2) return {false, "用法：write_file <文件名> <内容>", false};
        const auto& name = command.arguments[0];
        const auto content = decodeEscapeSequences(extractWriteFileContent(command));
        std::string message; const bool ok = vfs_.writeFile(owner, name, content, message);
        return {ok, message, false};
    }
    if (command.name == "read_file") {
        if (command.arguments.size() != 1) return {false, "用法：read_file <文件名>", false};
        return {true, vfs_.readFile(owner, command.arguments[0]), false};
    }
    if (command.name == "ls_file") {
        if (!command.arguments.empty()) return {false, "用法：ls_file", false};
        return {true, vfs_.listFiles(owner), false};
    }
    if (command.name == "rm_file") {
        if (command.arguments.size() != 1) return {false, "用法：rm_file <文件名>", false};
        std::string message; const bool ok = vfs_.deleteFile(owner, command.arguments[0], message);
        return {ok, message, false};
    }
    return {false, "[错误] 未知 VFS 命令。", false};
}

bool Kernel::isVfsCommand(const std::string& name) const { return name == "touch_file" || name == "write_file" || name == "read_file" || name == "ls_file" || name == "rm_file"; }
bool Kernel::isResetCommand(const std::string& name) const { return name == "reset_system"; }

// 导出完整系统状态快照（在 stateMutex_ 持锁下调用）。
// 覆盖：用户账户、PCB 表、就绪队列、内存块表、调度状态、VFS 文件。
// 各子系统提供独立的 export 方法，Kernel 负责收集组合。
KernelSnapshot Kernel::exportSnapshotLocked() const {
    KernelSnapshot snapshot;
    snapshot.users = userManager_.exportUsers(); snapshot.nextPid = processManager_.exportNextPid();
    snapshot.pcbs = processManager_.exportPcbs(); snapshot.readyQueues = processManager_.exportReadyQueues();
    snapshot.memoryBlocks = memoryManager_.exportBlocks(); snapshot.totalMemoryKB = memoryManager_.totalMemoryKB();
    snapshot.allocAlgorithm = memoryManager_.currentAlgorithm();
    snapshot.schedulerRunning = schedulerRunning_.load(); snapshot.schedulerOwner = schedulerOwner_;
    snapshot.nextFileId = vfs_.exportNextFileId(); snapshot.virtualFiles = vfs_.exportFiles();
    return snapshot;
}

bool Kernel::importSnapshotLocked(const KernelSnapshot& snapshot, std::string& message, bool preserveCurrentSession) {
    if (!validateSnapshot(snapshot, message)) return false;
    schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
    const auto previousUser = preserveCurrentSession ? userManager_.currentUser() : std::string{};
    userManager_.importUsers(snapshot.users);
    if (preserveCurrentSession) userManager_.restoreSessionIfUserExists(previousUser);
    else userManager_.clearCurrentSession();
    processManager_.importPcbs(snapshot.pcbs); processManager_.importNextPid(snapshot.nextPid);
    processManager_.importReadyQueues(snapshot.readyQueues); processManager_.rebuildParentChildRelationsIfNeeded();
    std::string queueMessage; processManager_.validateReadyQueues(queueMessage);
    memoryManager_.setTotalMemoryKB(snapshot.totalMemoryKB); memoryManager_.setAlgorithmDirect(snapshot.allocAlgorithm);
    memoryManager_.importBlocks(snapshot.memoryBlocks);
    std::string memoryMessage;
    if (!memoryManager_.validateBlocks(memoryMessage)) { message = memoryMessage; return false; }
    vfs_.importNextFileId(snapshot.nextFileId); vfs_.importFiles(snapshot.virtualFiles);
    autoLoadStatus_ = "手动加载成功";
    message = queueMessage; return true;
}

void Kernel::resetStateLocked() {
    userManager_.importUsers({}); userManager_.clearCurrentSession();
    processManager_.importPcbs({}); processManager_.importNextPid(1); processManager_.importReadyQueues({});
    memoryManager_.setTotalMemoryKB(1024); memoryManager_.setAlgorithmDirect(AllocAlgorithm::FIRST_FIT);
    vfs_.importNextFileId(1); vfs_.importFiles({});
    memoryManager_.importBlocks({MemoryBlock{0, 1024, MemoryBlockType::FREE, 0, "", ""}});
    schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
}

bool Kernel::validateSnapshot(const KernelSnapshot& snapshot, std::string& message) const {
    std::unordered_set<std::string> usernames;
    for (const auto& account : snapshot.users) {
        if (account.username.empty()) { message = "快照无效: 用户名为空。"; return false; }
        if (!usernames.insert(account.username).second) { message = "快照无效: 重复的用户名。"; return false; }
    }
    std::unordered_map<std::uint32_t, PCB> pcbs; std::uint32_t maxPid = 0;
    for (const auto& pcb : snapshot.pcbs) {
        if (pcb.pid == 0 || !pcbs.emplace(pcb.pid, pcb).second) { message = "快照无效: 重复或为零的 PID。"; return false; }
        if (usernames.find(pcb.owner) == usernames.end()) { message = "快照无效: PCB 所有者不存在。"; return false; }
        if (pcb.ppid == pcb.pid) { message = "快照无效: PCB 不能是自己的父进程。"; return false; }
        if (pcb.priority < 0 || pcb.priority > 15 || pcb.queueLevel < 0 || pcb.queueLevel > 2) { message = "快照无效: PCB 优先级或队列等级超出范围。"; return false; }
        if (pcb.executedTime > pcb.totalTime || pcb.remainingTime > pcb.totalTime) { message = "快照无效: PCB 运行时间计数器超出范围。"; return false; }
        maxPid = std::max(maxPid, pcb.pid);
    }
    if (snapshot.nextPid <= maxPid) { message = "快照无效: nextPid 必须大于现有 PID。"; return false; }
    for (const auto& pcb : snapshot.pcbs) {
        if (pcb.ppid != 0 && pcbs.find(pcb.ppid) == pcbs.end()) { message = "快照无效: 父 PID 不存在。"; return false; }
        for (const auto child : pcb.children) { if (pcbs.find(child) == pcbs.end()) { message = "快照无效: 子 PID 不存在。"; return false; } }
    }
    if (snapshot.totalMemoryKB == 0) { message = "快照无效: 总内存必须大于 0。"; return false; }
    std::unordered_map<std::uint32_t, const MemoryBlock*> processMemoryByPid;
    for (const auto& block : snapshot.memoryBlocks) {
        if (block.type == MemoryBlockType::PROCESS) {
            auto process = pcbs.find(block.pid);
            if (process == pcbs.end() || process->second.owner != block.owner) { message = "快照无效: PROCESS 内存块与 PCB 不匹配。"; return false; }
            if (process->second.swappedOut || process->second.state == ProcessState::SWAPPED) { message = "快照无效: 换出的 PCB 不能有 PROCESS 内存块。"; return false; }
            if (block.start != process->second.memStart || block.size != process->second.memSize) { message = "快照无效: PROCESS 内存块与 PCB 内存字段不匹配。"; return false; }
            if (!processMemoryByPid.emplace(block.pid, &block).second) { message = "快照无效: 重复的 PROCESS 内存块 PID。"; return false; }
        }
        if ((block.type == MemoryBlockType::KERNEL || block.type == MemoryBlockType::PROCESS) && usernames.find(block.owner) == usernames.end()) { message = "快照无效: 内存所有者不存在。"; return false; }
    }
    for (const auto& [pid, pcb] : pcbs) {
        const bool swapped = pcb.swappedOut || pcb.state == ProcessState::SWAPPED;
        if (!swapped && processMemoryByPid.find(pid) == processMemoryByPid.end()) { message = "快照无效: 活动的 PCB 缺少 PROCESS 内存块。"; return false; }
    }
    MemoryManager memoryValidator; memoryValidator.setTotalMemoryKB(snapshot.totalMemoryKB);
    memoryValidator.setAlgorithmDirect(snapshot.allocAlgorithm); memoryValidator.importBlocks(snapshot.memoryBlocks);
    return memoryValidator.validateBlocks(message);
}

std::string Kernel::snapshotSummaryText(const KernelSnapshot& snapshot) const {
    const auto summary = snapshotStore_.summarize(snapshot);
    std::ostringstream output;
    output << "用户: " << summary.users << '\n'
           << "进程: " << summary.processes << '\n'
           << "内存块: " << summary.memoryBlocks << '\n'
           << "就绪队列: Q0=" << summary.readyQueueSizes[0]
           << ", Q1=" << summary.readyQueueSizes[1]
           << ", Q2=" << summary.readyQueueSizes[2];
    return output.str();
}

} // namespace oscore
