#include "ipc/NamedPipeServer.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace oscore {

NamedPipeServer::~NamedPipeServer() {
    stop();
}

bool NamedPipeServer::start(CommandHandler handler) {
    if (running_.load()) {
        return false;
    }

    handler_ = std::move(handler);
    running_.store(true);
    serverThread_ = std::thread(&NamedPipeServer::serverLoop, this);
    return true;
}

void NamedPipeServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // 通过连接自身管道来唤醒阻塞在 ConnectNamedPipe 上的服务器线程
    // 避免线程无限期阻塞导致程序无法退出
    HANDLE wakePipe = CreateFileA(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (wakePipe != INVALID_HANDLE_VALUE) {
        CloseHandle(wakePipe);
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

bool NamedPipeServer::isRunning() const {
    return running_.load();
}

void NamedPipeServer::serverLoop() {
    // 预创建第一个管道实例，确保在进入循环之前管道名称已存在
    HANDLE pipe = CreateNamedPipeA(
        kPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        kBufferSize,
        kBufferSize,
        0,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        return;
    }

    while (running_.load()) {
        // 阻塞等待客户端连接
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            pipe = CreateNamedPipeA(kPipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kBufferSize, kBufferSize, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                Sleep(100);
                pipe = CreateNamedPipeA(kPipeName,
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                    PIPE_UNLIMITED_INSTANCES,
                    kBufferSize, kBufferSize, 0, nullptr);
            }
            continue;
        }

        if (!running_.load()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        // 读取客户端发来的命令字符串
        const auto command = readMessage(pipe);
        if (!command.empty()) {
            // 调用 Kernel 命令处理器执行命令，获取响应
            const auto response = handler_(command);

            // 将响应写回客户端（如果写失败，客户端会检测到断连）
            [[maybe_unused]] const bool writeOk = writeMessage(pipe, response);
            FlushFileBuffers(pipe);
        }

        // 【关键】在关闭当前管道之前预创建下一个管道实例
        // 这样任何时候管道名称都存在，Client 的 WaitNamedPipeA 不会超时
        HANDLE nextPipe = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kBufferSize,
            kBufferSize,
            0,
            nullptr);

        // 现在安全关闭当前管道
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        pipe = nextPipe;

        // 如果创建失败，短暂等待后重试
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(50);
            pipe = CreateNamedPipeA(kPipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kBufferSize, kBufferSize, 0, nullptr);
        }
    }

    // 清理最后的管道句柄
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
}

std::string NamedPipeServer::readMessage(HANDLE pipe) {
    // 先读取 4 字节的消息长度（little-endian uint32_t）
    std::uint32_t length = 0;
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, &length, sizeof(length), &bytesRead, nullptr) ||
        bytesRead != sizeof(length) ||
        length == 0) {
        return {};
    }

    // 读取指定长度的消息体
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

bool NamedPipeServer::writeMessage(HANDLE pipe, const std::string& message) {
    // 先写入 4 字节的消息长度
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
