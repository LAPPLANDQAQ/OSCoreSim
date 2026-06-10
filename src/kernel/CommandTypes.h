#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace oscore {

// 命令来源枚举：区分本地控制台输入和远程 NamedPipe 客户端。
enum class CommandSource {
    LocalConsole,   // 本地终端交互输入
    RemoteClient    // 通过 Named Pipe 转发到 Master 的命令
};

// Command：解析后的命令结构。
// 保留 rawLine 原始字符串，方便 write_file 等需要带空格内容的命令从原始行中提取正文。
// name 已转为小写（与命令关键字匹配），arguments 按空白拆分。
struct Command {
    std::string rawLine;                    // 原始输入行（未经处理）
    std::string name;                       // 命令名（小写）
    std::vector<std::string> arguments;     // 参数列表（按空格拆分）

    [[nodiscard]] bool empty() const { return name.empty(); } // name 为空表示用户输入为空行或全空白。
};

// CommandRequest：前台线程创建、放入 BlockingQueue 的请求包。
// 后台 worker 线程执行后通过 promise 将响应传回前台，前台通过 future.get() 同步等待。
struct CommandRequest {
    std::uint64_t id = 0;                                      // 请求编号（递增）
    std::string rawLine;                                       // 原始命令行
    std::string username;                                      // 请求发起时的登录用户
    CommandSource source = CommandSource::LocalConsole;        // 命令来源
    std::shared_ptr<std::promise<struct CommandResponse>> promise;  // Promise 用于回传结果
};

// CommandResponse：命令执行结果。
// success=false 表示命令执行失败（如参数错误、登录要求等）
// shouldExit=true 表示 exit/quit 命令已触发关闭
struct CommandResponse {
    bool success = false;       // 命令是否成功执行
    std::string message;        // 响应文本（错误信息或成功输出）
    bool shouldExit = false;    // 是否应触发程序退出
};

// CommandContext：只读上下文，供 Dispatcher 的 status 等命令展示当前内核运行状态。
// 由 Kernel::executeRequest 在持锁期间构造，传入 CommandDispatcher::dispatch。
struct CommandContext {
    std::uint64_t requestId = 0;             // 本次请求编号，用于状态输出和调试追踪。
    std::string username;                    // 当前登录用户（空 = 未登录）
    CommandSource source = CommandSource::LocalConsole;
    bool workerRunning = false;              // worker 线程是否运行中
    bool schedulerRunning = false;           // 自动调度器是否运行中
    std::string schedulerOwner;              // 调度器所属用户
    std::uint32_t schedulerIntervalMs = 500; // 自动调度间隔（毫秒）
    std::string snapshotPath;                // 快照文件路径
    std::string autoLoadStatus;              // 启动时自动加载快照的状态描述
};

} // namespace oscore
