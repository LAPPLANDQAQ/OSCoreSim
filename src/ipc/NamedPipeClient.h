#pragma once

#include <string>
#include <windows.h>

namespace oscore {

// NamedPipeClient —— Client 实例用于向 Master 发送命令并接收响应
//
// 每次调用 sendCommand 都会：
//   1. 连接到 \\.\pipe\OS_SIM_PIPE_2026
//   2. 发送命令（长度前缀协议）
//   3. 接收响应（长度前缀协议）
//   4. 断开连接
//
// 如果 Master 尚未就绪（管道不存在），会等待最多 2 秒后返回错误。
class NamedPipeClient {
public:
    // 发送命令到 Master，接收响应。
    // command: 原始命令字符串（如 "create_pcb init 64 0 20"）
    // response: 输出参数，Master 返回的响应字符串
    // 返回 true 表示成功，false 表示通信失败
    [[nodiscard]] bool sendCommand(const std::string& command, std::string& response);

private:
    // 读取一条长度前缀消息
    [[nodiscard]] static std::string readMessage(HANDLE pipe);

    // 写入一条长度前缀消息
    [[nodiscard]] static bool writeMessage(HANDLE pipe, const std::string& message);

    static constexpr const char* kPipeName = "\\\\.\\pipe\\OS_SIM_PIPE_2026";
    static constexpr DWORD kTimeoutMs = 2000;  // 等待 MASTER 管道就绪的最长时间。
};

} // namespace oscore
