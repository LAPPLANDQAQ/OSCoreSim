#include "ipc/NamedPipeClient.h"

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace oscore {

bool NamedPipeClient::sendCommand(const std::string& command, std::string& response) {
    // 等待管道可用（Master 可能尚未完全启动）
    if (!WaitNamedPipeA(kPipeName, kTimeoutMs)) {
        response = "[ERROR] Cannot connect to Kernel Master. "
                   "Please make sure the first instance is running.";
        return false;
    }

    // 连接到 Master 的命名管道
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
        CloseHandle(pipe);
        response = "[ERROR] Failed to set pipe read mode (error="
                   + std::to_string(GetLastError()) + ").";
        return false;
    }

    // 发送命令
    if (!writeMessage(pipe, command)) {
        CloseHandle(pipe);
        response = "[ERROR] Failed to send command to Master.";
        return false;
    }

    // 接收响应
    const auto result = readMessage(pipe);
    CloseHandle(pipe);

    if (result.empty()) {
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
        return {};
    }

    // 读取消息体
    std::vector<char> buffer(length);
    DWORD totalRead = 0;
    while (totalRead < length) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, buffer.data() + totalRead, length - totalRead, &chunk, nullptr) ||
            chunk == 0) {
            return {};
        }
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
        return false;
    }

    // 写入消息体
    DWORD totalWritten = 0;
    const char* data = message.data();
    while (totalWritten < length) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, data + totalWritten, length - totalWritten, &chunk, nullptr)) {
            return false;
        }
        totalWritten += chunk;
    }

    return true;
}

} // namespace oscore
