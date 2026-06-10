#include "ipc/NamedPipeClient.h"

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace oscore {

bool NamedPipeClient::sendCommand(const std::string& command, std::string& response) {
    // 等待管道可用（Master 可能尚未完全启动）
    if (!WaitNamedPipeA(kPipeName, kTimeoutMs)) {
        // 等待超时通常表示 MASTER 未启动或管道线程未成功创建。
        response = "[ERROR] Cannot connect to Kernel Master. "
                   "Please make sure the first instance is running.";
        return false;
    }

    // 连接到 Master 的命名管道
    // CLIENT 只持有本次请求的 HANDLE，不拥有 Kernel 状态，也不直接访问快照文件。
    HANDLE pipe = CreateFileA(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,                          // 不共享
        nullptr,                    // 默认安全属性
        OPEN_EXISTING,              // 打开已有管道
        0,                          // 默认属性
        nullptr);                   // 无模板文件

    if (pipe == INVALID_HANDLE_VALUE) {
        response = "[ERROR] Cannot connect to Kernel Master (CreateFileA failed, error="
                   + std::to_string(GetLastError()) + ").";
        return false;
    }

    // 设置为消息读模式，与服务器端的 PIPE_TYPE_MESSAGE 匹配
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        // 模式设置失败时继续通信可能破坏消息边界，因此立即关闭。
        CloseHandle(pipe);
        response = "[ERROR] Failed to set pipe read mode (error="
                   + std::to_string(GetLastError()) + ").";
        return false;
    }

    // 发送命令。Client 不直接访问 Kernel 或 data/os_state.bin，只负责 IPC 转发。
    if (!writeMessage(pipe, command)) {
        // 发送失败时关闭本次连接，调用方会退出 CLIENT 循环或提示错误。
        CloseHandle(pipe);
        response = "[ERROR] Failed to send command to Master.";
        return false;
    }

    // 接收响应
    const auto result = readMessage(pipe);
    // 每次请求使用短连接，收到响应后立即关闭 HANDLE。
    CloseHandle(pipe);

    if (result.empty()) {
        // 空响应被视为通信失败，避免把失败误当成命令正常无输出。
        response = "[ERROR] Failed to receive response from Master.";
        return false;
    }

    response = result;
    return true;
}

std::string NamedPipeClient::readMessage(HANDLE pipe) {
    // 读取 4 字节消息长度
    std::uint32_t length = 0;
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, &length, sizeof(length), &bytesRead, nullptr) ||
        bytesRead != sizeof(length) ||
        length == 0) {
        // 无法读取长度前缀时无法继续解析消息。
        return {};
    }

    // 读取消息体
    // 响应长度由服务端前缀指定，循环读取可避免 overview 等长输出被固定缓冲区截断。
    std::vector<char> buffer(length);
    DWORD totalRead = 0;
    while (totalRead < length) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, buffer.data() + totalRead, length - totalRead, &chunk, nullptr) ||
            chunk == 0) {
            // 响应体读取中断则整条响应失败。
            return {};
        }
        // 持续读取直到收到 length 指定的全部字节。
        totalRead += chunk;
    }

    return std::string(buffer.data(), length);
}

bool NamedPipeClient::writeMessage(HANDLE pipe, const std::string& message) {
    // 写入 4 字节消息长度
    const auto length = static_cast<std::uint32_t>(message.size());
    DWORD bytesWritten = 0;
    if (!WriteFile(pipe, &length, sizeof(length), &bytesWritten, nullptr) ||
        bytesWritten != sizeof(length)) {
        // 长度前缀写失败，服务端无法知道命令体大小。
        return false;
    }

    // 写入消息体
    // 命令同样采用 length + bytes 协议，与服务端读写函数严格对应。
    DWORD totalWritten = 0;
    const char* data = message.data();
    while (totalWritten < length) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, data + totalWritten, length - totalWritten, &chunk, nullptr)) {
            return false;
        }
        // 累计写入字节数，直到命令体完整发送。
        totalWritten += chunk;
    }

    return true;
}

} // namespace oscore
