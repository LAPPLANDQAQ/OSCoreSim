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

// 从 write_file 原始命令行中提取“文件内容”部分。
// 解析后的 arguments 会按空格拆开，因此包含空格的正文必须从 rawLine 重新定位。
std::string extractWriteFileContent(const Command& command) {
    const auto& line = command.rawLine;
    // 跳过命令名前的空白。
    auto pos = line.find_first_not_of(" \t\r\n");
    if (pos == std::string::npos) { return {}; }
    // 跳过命令名 write_file。
    pos = line.find_first_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    // 跳过命令名后的空白，定位文件名。
    pos = line.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    // 跳过文件名。
    pos = line.find_first_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    // 跳过文件名后的空白，剩余部分就是文件内容。
    pos = line.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string::npos) { return {}; }
    return line.substr(pos);
}

// 将菜单层或原始命令中的转义序列还原成真实文件内容。
std::string decodeEscapeSequences(const std::string& raw) {
    if (raw.empty()) return raw;
    std::string result;
    // 结果长度不会超过原始长度，提前 reserve 可减少多行内容写入时的重分配。
    result.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            // 识别一小组命令行安全转义，其他反斜杠组合保留原样的反斜杠。
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

// 析构时调用 stop()，保证异常退出作用域时后台线程也能被 join。
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

    requestQueue_.reset();                // 清空上次运行残留的请求
    stopping_ = false;
    shuttingDown_.store(false);
    schedulerRunning_.store(false);       // 启动时调度器默认停止
    scheduler_.setRunning(false);
    schedulerOwner_.clear();
    resetStateLocked();                   // 恢复所有子系统到初始状态

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
            resetStateLocked();            // 快照损坏 → 回退到干净状态
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
    workerThread_ = std::thread(&Kernel::workerLoop, this);         // 启动命令执行线程
    schedulerThread_ = std::thread(&Kernel::schedulerLoop, this);   // 启动自动调度线程
}

void Kernel::stop() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        // 如果从未启动且线程不可 join，则 stop() 是幂等空操作。
        if (!started_ && !workerThread_.joinable() && !schedulerThread_.joinable()) return;
        stopping_ = true;
        schedulerRunning_.store(false);
        scheduler_.setRunning(false);
        schedulerOwner_.clear();
        // 通知 schedulerLoop 自然跳出 while 循环。
        shuttingDown_.store(true);
    }
    requestQueue_.shutdown();              // 唤醒阻塞中的 worker 线程
    // 先等待调度线程，再等待命令线程，确保没有后台代码继续访问子系统。
    if (schedulerThread_.joinable()) schedulerThread_.join();
    if (workerThread_.joinable()) workerThread_.join();
    std::lock_guard<std::mutex> lock(stateMutex_);
    started_ = false; workerRunning_ = false;
}

CommandResponse Kernel::submitCommand(const std::string& rawLine, const std::string& username, CommandSource source) {
    // promise/future 让提交线程可以同步等待 worker 线程的命令执行结果。
    auto promise = std::make_shared<std::promise<CommandResponse>>();
    auto future = promise->get_future();
    CommandRequest request;
    // rawLine 保持原样，username/source 记录命令提交时的上下文。
    request.rawLine = rawLine; request.username = username; request.source = source;
    request.promise = std::move(promise);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        // 停止中不再接受新命令，避免关闭流程中继续修改状态。
        if (!started_ || stopping_) return {false, "[错误] 内核未运行。", false};
        request.id = nextRequestId_++;
    }
    try { requestQueue_.push(std::move(request)); }  // 入队后唤醒 worker 线程
    catch (const std::exception& ex) {
        std::ostringstream output;
        output << "[错误] 提交命令失败: " << ex.what();
        return {false, output.str(), false};
    }
    return future.get();  // 阻塞等待 worker 线程通过 promise 回传结果
}

// 读取 workerRunning_ 需要持锁，因为它由 workerLoop 在线程启动/结束时写入。
bool Kernel::isWorkerRunning() const { std::lock_guard<std::mutex> lock(stateMutex_); return workerRunning_; }
// 登录状态由 UserManager 管理；这里只做只读转发。
bool Kernel::isLoggedIn() const { return userManager_.isLoggedIn(); }
// currentUser() 供 ConsoleApp/NamedPipeServer 在提交命令前记录当前会话。
std::string Kernel::currentUser() const { return userManager_.currentUser(); }
// 启动消息可能在 start() 中写入，读取时持锁避免并发访问。
std::string Kernel::startupMessage() const { std::lock_guard<std::mutex> lock(stateMutex_); return startupMessage_; }
// 导出快照必须持锁，保证多个子系统在同一时刻被一致地读取。
KernelSnapshot Kernel::exportSnapshot() const { std::lock_guard<std::mutex> lock(stateMutex_); return exportSnapshotLocked(); }
// 外部导入快照也通过 Kernel 串行执行，避免绕过 validateSnapshot。
bool Kernel::importSnapshot(const KernelSnapshot& snapshot, std::string& message) { std::lock_guard<std::mutex> lock(stateMutex_); return importSnapshotLocked(snapshot, message, false); }

// workerLoop：命令执行线程主循环。
// 从 BlockingQueue 阻塞获取命令 → executeRequest() 串行执行 → 通过 promise 回传结果。
// 收到 shouldExit 响应后关闭队列并退出循环。
// 所有命令通过 stateMutex_ 串行执行，避免并发修改内核状态。
void Kernel::workerLoop() {
    // workerRunning_ 是状态查询字段，写入时受 stateMutex_ 保护。
    { std::lock_guard<std::mutex> lock(stateMutex_); workerRunning_ = true; }
    CommandRequest request;
    while (requestQueue_.pop(request)) {                // 阻塞等待新命令
        auto response = executeRequest(request);         // 持 stateMutex_ 串行执行
        if (request.promise) request.promise->set_value(response);  // 回传结果给前台
        // exit/quit 的响应会关闭队列，使后续 pop 返回 false。
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
        if (!schedulerRunning_.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }  // 未启动时低频检查
        std::string log;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);     // 与命令线程共用同一把锁
            if (schedulerRunning_.load() && !schedulerOwner_.empty())
                log = scheduler_.step(schedulerOwner_, processManager_, memoryManager_);  // 执行一次 MLFQ step
        }
        if (!log.empty()) { std::lock_guard<std::mutex> consoleLock(consoleMutex_); std::cout << "\n[自动调度]\n" << log << std::endl; }
        // 运行中按配置间隔调度；停止时上方分支使用更短的 100ms 检查间隔。
        std::this_thread::sleep_for(std::chrono::milliseconds(schedulerIntervalMs_));
    }
}

CommandResponse Kernel::executeRequest(const CommandRequest& request) {
    try {
        const auto command = dispatcher_.parse(request.rawLine);  // 拆词：命令名 + 参数列表
        std::lock_guard<std::mutex> lock(stateMutex_);            // 串行执行，保护所有子系统
        const CommandContext context{request.id, userManager_.currentUser(), request.source, workerRunning_,
            schedulerRunning_.load(), schedulerOwner_, schedulerIntervalMs_,
            snapshotStore_.defaultPath(), autoLoadStatus_};
        CommandResponse response;
        // 按优先级路由：调度 > 持久化 > 可视化 > VFS > 复位 > 通用 Dispatcher
        if (isSchedulerCommand(command.name)) response = handleSchedulerCommand(command, context);
        else if (isPersistenceCommand(command.name)) response = handlePersistenceCommand(command);
        else if (isVisualizationCommand(command.name)) response = handleOverview(command);
        else if (isVfsCommand(command.name)) response = handleVfsCommand(command);
        else if (isResetCommand(command.name)) response = handleResetCommand(command);
        else response = dispatcher_.dispatch(command, context, userManager_, processManager_, memoryManager_);
        // logout 或 exit 后自动停止调度器
        if (response.shouldExit || (response.success && command.name == "logout")) {
            schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
        }
        return response;
    } catch (const std::exception& ex) {
        std::ostringstream output;
        // 防止单条命令异常导致 worker 线程退出。
        output << "[错误] 命令执行失败: " << ex.what();
        return {false, output.str(), false};
    }
}

// 调度命令由 Kernel 处理，因为它们需要控制 schedulerRunning_ 和调度线程共享状态。
bool Kernel::isSchedulerCommand(const std::string& name) const { return name == "start_sched" || name == "start" || name == "stop_sched" || name == "stop" || name == "restart_sched" || name == "restart" || name == "step"; }
// save/load 需要汇总或替换所有子系统状态，只能由 Kernel 处理。
bool Kernel::isPersistenceCommand(const std::string& name) const { return name == "save" || name == "load"; }
// overview 需要跨模块采集状态，只能由 Kernel 组装数据后调用渲染器。
bool Kernel::isVisualizationCommand(const std::string& name) const { return name == "overview"; }

CommandResponse Kernel::handleSchedulerCommand(const Command& command, const CommandContext& context) {
    (void)context;
    // 调度器按当前登录用户的进程集合运行，未登录时没有 owner。
    if (!userManager_.isLoggedIn()) return {false, "[提示] 当前命令需要先登录。用法：login <用户名> <密码>", false};
    const auto owner = userManager_.currentUser();
    std::string schedulerCommand = command.name;
    // 别名映射：start→start_sched  stop→stop_sched  restart→restart_sched
    if (schedulerCommand == "start") schedulerCommand = "start_sched";
    else if (schedulerCommand == "stop") schedulerCommand = "stop_sched";
    else if (schedulerCommand == "restart") schedulerCommand = "restart_sched";

    if (schedulerCommand == "step") {
        // 单步调度不启动后台循环，只立即执行一次 Scheduler::step。
        if (!command.arguments.empty()) return {false, "用法：step", false};
        return {true, scheduler_.step(owner, processManager_, memoryManager_), false};
    }
    if (schedulerCommand == "start_sched") {
        // start_sched 只接受零参数，避免误把其他文本当成配置。
        if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
        if (schedulerRunning_.load()) return {true, "[提示] MLFQ 调度器已在为用户 " + schedulerOwner_ + " 运行。", false};
        // 记录调度归属用户，并打开原子运行标志，schedulerLoop 会在下一轮执行 step。
        schedulerOwner_ = owner; schedulerRunning_.store(true); scheduler_.setRunning(true);
        std::ostringstream output;
        output << "[成功] MLFQ 调度器已为用户 " << schedulerOwner_ << " 启动。\n自动调度间隔: " << schedulerIntervalMs_ << "ms。";
        return {true, output.str(), false};
    }
    if (schedulerCommand == "stop_sched") {
        if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
        if (!schedulerRunning_.load()) return {true, "[提示] 调度器未运行。", false};
        // 关闭运行标志后，schedulerLoop 会停止执行 step，但线程仍保留等待重启。
        schedulerRunning_.store(false); scheduler_.setRunning(false);
        std::ostringstream output;
        output << "[成功] 调度器已停止。\n当前就绪队列:\n" << processManager_.readyQueueSnapshot(owner);
        return {true, output.str(), false};
    }
    if (schedulerCommand == "restart_sched") {
        if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
        // 重启前先停止，避免清理就绪队列时自动调度同时运行。
        schedulerRunning_.store(false); scheduler_.setRunning(false);
        const auto removed = processManager_.cleanupInvalidReadyQueueEntries(owner);
        // 清理无效队列条目后重新指定 owner 并启动。
        schedulerOwner_ = owner; schedulerRunning_.store(true); scheduler_.setRunning(true);
        std::ostringstream output;
        output << "[成功] 调度器已重启。\n已移除无效就绪队列条目: " << removed.size() << "\n调度用户: " << schedulerOwner_;
        return {true, output.str(), false};
    }
    return {false, "[错误] 未知调度命令。", false};
}

CommandResponse Kernel::handlePersistenceCommand(const Command& command) {
    // save/load 都不接受额外参数，快照路径由 SnapshotStore 默认路径决定。
    if (!command.arguments.empty()) return {false, "用法：" + command.name, false};
    if (command.name == "save") {
        const auto snapshot = exportSnapshotLocked();  // 导出当前全部系统状态
        std::string message;
        if (!snapshotStore_.save(snapshot, message)) return {false, message, false};  // 原子写入快照文件
        std::ostringstream output;
        output << "[成功] 系统状态已保存到 " << snapshotStore_.defaultPath() << '\n' << snapshotSummaryText(snapshot);
        return {true, output.str(), false};
    }
    if (command.name == "load") {
        // 加载快照前先停止自动调度，防止调度线程使用即将被替换的 PCB/内存状态。
        schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
        // 手动 load 尝试保留当前登录用户；如果快照里不存在该用户则清除会话。
        const auto previousUser = userManager_.currentUser();
        KernelSnapshot snapshot; std::string message;
        if (!snapshotStore_.load(snapshot, message)) return {false, message, false};
        // importSnapshotLocked 会先校验快照，再整体替换各子系统状态。
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
    // overview 是课程验收用总览，必须以当前登录用户为视角渲染。
    if (!userManager_.isLoggedIn()) return {false, "[错误] 请先登录再使用 overview。", false};
    if (!command.arguments.empty()) return {false, "用法：overview", false};
    const auto currentUser = userManager_.currentUser();
    // 下面逐项从子系统导出只读副本，避免渲染器直接修改核心状态。
    const auto userProcesses = processManager_.getProcessCopiesForUser(currentUser);
    const auto readyQueues = processManager_.exportReadyQueues();
    const auto memoryBlocks = memoryManager_.exportBlocks();
    const auto totalMemKB = memoryManager_.totalMemoryKB();
    const auto vfsFileCount = vfs_.fileCountForUser(currentUser);
    OverviewRenderer::SchedulerInfo schedulerInfo;
    schedulerInfo.running = schedulerRunning_.load(); schedulerInfo.owner = schedulerOwner_;
    schedulerInfo.intervalMs = static_cast<int>(schedulerIntervalMs_);
    // OverviewRenderer 只负责字符串渲染，所有数据源都由 Kernel 显式传入。
    const auto output = overviewRenderer_.render(currentUser, userProcesses, readyQueues, memoryBlocks, totalMemKB, schedulerInfo, snapshotStore_.defaultPath(), memoryManager_.currentAlgorithmName(), vfsFileCount);
    return {true, output, false};
}

CommandResponse Kernel::handleResetCommand(const Command& command) {
    // reset_system 不接受参数，避免误删或误替换外部文件。
    if (!command.arguments.empty()) return {false, "用法：reset_system", false};
    // 重置前先关闭调度器，确保不会在状态清空后继续 step。
    schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
    resetStateLocked();
    return {true, "[成功] 系统已重置到干净状态。", false};
}

CommandResponse Kernel::handleVfsCommand(const Command& command) {
    // VFS 按用户隔离，所有文件操作都必须以当前登录用户为 owner。
    if (!userManager_.isLoggedIn()) return {false, "[提示] 当前命令需要先登录。用法：login <用户名> <密码>", false};
    const auto owner = userManager_.currentUser();
    if (command.name == "touch_file") {
        // touch_file 只创建空文件，不写入内容。
        if (command.arguments.size() != 1) return {false, "用法：touch_file <文件名>", false};
        std::string message; const bool ok = vfs_.createFile(owner, command.arguments[0], message);
        return {ok, message, false};
    }
    if (command.name == "write_file") {
        // write_file 的正文可包含空格，因此不能直接拼接 command.arguments[1...]。
        if (command.arguments.size() < 2) return {false, "用法：write_file <文件名> <内容>", false};
        const auto& name = command.arguments[0];
        // 从 rawLine 提取正文后再解码 \n、\t 等转义。
        const auto content = decodeEscapeSequences(extractWriteFileContent(command));
        std::string message; const bool ok = vfs_.writeFile(owner, name, content, message);
        return {ok, message, false};
    }
    if (command.name == "read_file") {
        // read_file 是只读操作，VFS 内部会检查文件是否属于 owner。
        if (command.arguments.size() != 1) return {false, "用法：read_file <文件名>", false};
        return {true, vfs_.readFile(owner, command.arguments[0]), false};
    }
    if (command.name == "ls_file") {
        // ls_file 不接受参数，始终列出当前用户文件。
        if (!command.arguments.empty()) return {false, "用法：ls_file", false};
        return {true, vfs_.listFiles(owner), false};
    }
    if (command.name == "rm_file") {
        // rm_file 删除指定文件名，实际存在性和权限由 VFS 判断。
        if (command.arguments.size() != 1) return {false, "用法：rm_file <文件名>", false};
        std::string message; const bool ok = vfs_.deleteFile(owner, command.arguments[0], message);
        return {ok, message, false};
    }
    return {false, "[错误] 未知 VFS 命令。", false};
}

// VFS 命令由 Kernel 处理，因为 VFS 不属于 CommandDispatcher 的用户/进程/内存三类基础命令。
bool Kernel::isVfsCommand(const std::string& name) const { return name == "touch_file" || name == "write_file" || name == "read_file" || name == "ls_file" || name == "rm_file"; }
// reset_system 会清空多个子系统状态，因此必须由 Kernel 处理。
bool Kernel::isResetCommand(const std::string& name) const { return name == "reset_system"; }

// 导出完整系统状态快照（在 stateMutex_ 持锁下调用）。
// 覆盖：用户账户、PCB 表、就绪队列、内存块表、调度状态、VFS 文件。
// 各子系统提供独立的 export 方法，Kernel 负责收集组合。
KernelSnapshot Kernel::exportSnapshotLocked() const {
    KernelSnapshot snapshot;
    // 用户账户与进程 PID 计数器共同决定后续登录和创建进程行为。
    snapshot.users = userManager_.exportUsers(); snapshot.nextPid = processManager_.exportNextPid();
    // PCB 表和 readyQueues_ 要一起保存，否则恢复后调度器不知道哪些进程可运行。
    snapshot.pcbs = processManager_.exportPcbs(); snapshot.readyQueues = processManager_.exportReadyQueues();
    // 内存块、总内存、分配算法共同还原动态分区管理器状态。
    snapshot.memoryBlocks = memoryManager_.exportBlocks(); snapshot.totalMemoryKB = memoryManager_.totalMemoryKB();
    snapshot.allocAlgorithm = memoryManager_.currentAlgorithm();
    // 保存调度器标志和 owner，但加载时会主动停止调度，避免恢复后后台自动运行。
    snapshot.schedulerRunning = schedulerRunning_.load(); snapshot.schedulerOwner = schedulerOwner_;
    // VFS 的 nextFileId 和文件列表一起保存，避免加载后文件 ID 重复。
    snapshot.nextFileId = vfs_.exportNextFileId(); snapshot.virtualFiles = vfs_.exportFiles();
    return snapshot;
}

bool Kernel::importSnapshotLocked(const KernelSnapshot& snapshot, std::string& message, bool preserveCurrentSession) {
    // 任何导入都先做跨模块一致性校验，失败时不替换当前状态。
    if (!validateSnapshot(snapshot, message)) return false;
    // 导入会整体替换进程/内存/VFS 状态，所以先停止调度器。
    schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
    // 手动 load 可尝试保留当前用户；启动自动加载则不保留旧会话。
    const auto previousUser = preserveCurrentSession ? userManager_.currentUser() : std::string{};
    // 先恢复用户表，因为 PCB、内存块、VFS 都会引用用户名。
    userManager_.importUsers(snapshot.users);
    if (preserveCurrentSession) userManager_.restoreSessionIfUserExists(previousUser);
    else userManager_.clearCurrentSession();
    // 恢复 PCB 表、nextPid 和 readyQueues_，保持调度结构与进程表一致。
    processManager_.importPcbs(snapshot.pcbs); processManager_.importNextPid(snapshot.nextPid);
    processManager_.importReadyQueues(snapshot.readyQueues); processManager_.rebuildParentChildRelationsIfNeeded();
    // 导入后清理/校验 readyQueues_ 中可能存在的失效 PID。
    std::string queueMessage; processManager_.validateReadyQueues(queueMessage);
    // 恢复内存容量、分配算法和分区列表。
    memoryManager_.setTotalMemoryKB(snapshot.totalMemoryKB); memoryManager_.setAlgorithmDirect(snapshot.allocAlgorithm);
    memoryManager_.importBlocks(snapshot.memoryBlocks);
    std::string memoryMessage;
    // 再次校验导入后的内存块，防止数据结构落地后仍不一致。
    if (!memoryManager_.validateBlocks(memoryMessage)) { message = memoryMessage; return false; }
    // 最后恢复 VFS，因为它只依赖用户名，不影响 PCB/内存校验。
    vfs_.importNextFileId(snapshot.nextFileId); vfs_.importFiles(snapshot.virtualFiles);
    autoLoadStatus_ = "手动加载成功";
    message = queueMessage; return true;
}

void Kernel::resetStateLocked() {
    // 清空用户表和当前登录会话。
    userManager_.importUsers({}); userManager_.clearCurrentSession();
    // 清空进程表，PID 从 1 重新开始，就绪队列也清空。
    processManager_.importPcbs({}); processManager_.importNextPid(1); processManager_.importReadyQueues({});
    // 内存恢复为 1024KB 和 FIRST_FIT，这是模拟器的初始配置。
    memoryManager_.setTotalMemoryKB(1024); memoryManager_.setAlgorithmDirect(AllocAlgorithm::FIRST_FIT);
    // VFS 清空文件并把下一个文件 ID 恢复为 1。
    vfs_.importNextFileId(1); vfs_.importFiles({});
    // 初始内存只有一整块空闲分区。
    memoryManager_.importBlocks({MemoryBlock{0, 1024, MemoryBlockType::FREE, 0, "", ""}});
    // 重置后调度器一定处于停止状态。
    schedulerRunning_.store(false); scheduler_.setRunning(false); schedulerOwner_.clear();
}

bool Kernel::validateSnapshot(const KernelSnapshot& snapshot, std::string& message) const {
    std::unordered_set<std::string> usernames;
    // 先收集用户表，后续 PCB、内存块都必须引用已存在用户。
    for (const auto& account : snapshot.users) {
        if (account.username.empty()) { message = "快照无效: 用户名为空。"; return false; }
        if (!usernames.insert(account.username).second) { message = "快照无效: 重复的用户名。"; return false; }
    }
    std::unordered_map<std::uint32_t, PCB> pcbs; std::uint32_t maxPid = 0;
    // 校验 PCB 基本字段，并建立 pid -> PCB 的查找表。
    for (const auto& pcb : snapshot.pcbs) {
        if (pcb.pid == 0 || !pcbs.emplace(pcb.pid, pcb).second) { message = "快照无效: 重复或为零的 PID。"; return false; }
        if (usernames.find(pcb.owner) == usernames.end()) { message = "快照无效: PCB 所有者不存在。"; return false; }
        if (pcb.ppid == pcb.pid) { message = "快照无效: PCB 不能是自己的父进程。"; return false; }
        if (pcb.priority < 0 || pcb.priority > 15 || pcb.queueLevel < 0 || pcb.queueLevel > 2) { message = "快照无效: PCB 优先级或队列等级超出范围。"; return false; }
        if (pcb.executedTime > pcb.totalTime || pcb.remainingTime > pcb.totalTime) { message = "快照无效: PCB 运行时间计数器超出范围。"; return false; }
        maxPid = std::max(maxPid, pcb.pid);
    }
    // nextPid 必须大于已存在最大 PID，避免加载后创建进程时复用 PID。
    if (snapshot.nextPid <= maxPid) { message = "快照无效: nextPid 必须大于现有 PID。"; return false; }
    // 再校验父子关系引用的 PID 是否都存在。
    for (const auto& pcb : snapshot.pcbs) {
        if (pcb.ppid != 0 && pcbs.find(pcb.ppid) == pcbs.end()) { message = "快照无效: 父 PID 不存在。"; return false; }
        for (const auto child : pcb.children) { if (pcbs.find(child) == pcbs.end()) { message = "快照无效: 子 PID 不存在。"; return false; } }
    }
    // 总内存为 0 会导致所有分区统计无意义，直接拒绝。
    if (snapshot.totalMemoryKB == 0) { message = "快照无效: 总内存必须大于 0。"; return false; }
    std::unordered_map<std::uint32_t, const MemoryBlock*> processMemoryByPid;
    // 校验 PROCESS 内存块必须与 PCB 一一对应。
    for (const auto& block : snapshot.memoryBlocks) {
        if (block.type == MemoryBlockType::PROCESS) {
            auto process = pcbs.find(block.pid);
            if (process == pcbs.end() || process->second.owner != block.owner) { message = "快照无效: PROCESS 内存块与 PCB 不匹配。"; return false; }
            if (process->second.swappedOut || process->second.state == ProcessState::SWAPPED) { message = "快照无效: 换出的 PCB 不能有 PROCESS 内存块。"; return false; }
            if (block.start != process->second.memStart || block.size != process->second.memSize) { message = "快照无效: PROCESS 内存块与 PCB 内存字段不匹配。"; return false; }
            if (!processMemoryByPid.emplace(block.pid, &block).second) { message = "快照无效: 重复的 PROCESS 内存块 PID。"; return false; }
        }
        // 内核/进程内存块如果带 owner，owner 必须来自用户表。
        if ((block.type == MemoryBlockType::KERNEL || block.type == MemoryBlockType::PROCESS) && usernames.find(block.owner) == usernames.end()) { message = "快照无效: 内存所有者不存在。"; return false; }
    }
    // 未换出的活动 PCB 必须有一块 PROCESS 内存。
    for (const auto& [pid, pcb] : pcbs) {
        const bool swapped = pcb.swappedOut || pcb.state == ProcessState::SWAPPED;
        if (!swapped && processMemoryByPid.find(pid) == processMemoryByPid.end()) { message = "快照无效: 活动的 PCB 缺少 PROCESS 内存块。"; return false; }
    }
    // 复用 MemoryManager 的分区连续性/重叠校验，避免 Kernel 复制内存校验规则。
    MemoryManager memoryValidator; memoryValidator.setTotalMemoryKB(snapshot.totalMemoryKB);
    memoryValidator.setAlgorithmDirect(snapshot.allocAlgorithm); memoryValidator.importBlocks(snapshot.memoryBlocks);
    return memoryValidator.validateBlocks(message);
}

std::string Kernel::snapshotSummaryText(const KernelSnapshot& snapshot) const {
    // SnapshotStore 负责统计快照摘要，Kernel 只把摘要转换为用户可读文本。
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
