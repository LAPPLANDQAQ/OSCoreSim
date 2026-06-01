#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <windows.h>

namespace oscore {

// NamedPipeServer —— Master 实例上的管道服务器
//
// 在独立线程中循环等待 Client 连接。
// 每次连接：读取命令 → 调用 CommandHandler → 返回响应 → 断开连接。
//
// 通信协议（长度前缀）：
//   Client → Server: uint32_t length + raw UTF-8 bytes（命令字符串）
//   Server → Client: uint32_t length + raw UTF-8 bytes（响应字符串）
//
// 管道名称：\\.\pipe\OS_SIM_PIPE_2026
class NamedPipeServer {
public:
    // 命令处理回调：接收原始命令字符串，返回响应字符串
    // 回调内部应通过 Kernel 的 submitCommand 执行，确保线程安全
    using CommandHandler = std::function<std::string(const std::string&)>;

    NamedPipeServer() = default;
    ~NamedPipeServer();

    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    // 启动管道服务器线程
    // handler: 命令处理回调，在 master 线程中调用
    // 返回 true 表示线程成功启动
    [[nodiscard]] bool start(CommandHandler handler);

    // 停止管道服务器
    // 通过连接自身管道来唤醒阻塞在 ConnectNamedPipe 上的服务器线程
    void stop();

    // 服务器是否正在运行
    [[nodiscard]] bool isRunning() const;

private:
    // 服务器主循环，运行在独立线程中
    void serverLoop();

    // 读取一条长度前缀消息，返回内容字符串。失败时返回空 optional。
    [[nodiscard]] static std::string readMessage(HANDLE pipe);

    // 写入一条长度前缀消息。返回 true 表示成功。
    [[nodiscard]] static bool writeMessage(HANDLE pipe, const std::string& message);

    static constexpr const char* kPipeName = "\\\\.\\pipe\\OS_SIM_PIPE_2026";
    static constexpr DWORD kBufferSize = 4096;

    CommandHandler handler_;
    std::thread serverThread_;
    std::atomic<bool> running_{false};
};

} // namespace oscore
