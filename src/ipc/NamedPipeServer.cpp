
#include "ipc/NamedPipeServer.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace oscore {

NamedPipeServer::~NamedPipeServer() {
    // 析构时确保管道线程停止，避免后台线程访问已销毁对象。
    stop();
}

bool NamedPipeServer::start(CommandHandler handler) {
    if (running_.load()) {
        // 已运行时不重复启动线程。
        return false;
    }

    // 保存 Kernel 命令处理回调。
    handler_ = std::move(handler);
    running_.store(true);
    // MASTER 进程启动后台管道线程，后续 CLIENT 命令都会通过 handler_ 进入 Kernel。
    serverThread_ = std::thread(&NamedPipeServer::serverLoop, this);
    return true;
}

void NamedPipeServer::stop() {
    if (!running_.load()) {
        // stop() 可重复调用，未运行时直接返回。
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

// serverLoop：管道服务器主循环。
// 生命周期：预创建管道 → 阻塞等待 Client 连接 → 读命令 → 调用 Kernel 执行 → 写响应 → 断开 → 预创建下一管道 → 循环。
// 预创建策略消除了管道实例创建和 Client 连接之间的时间间隙，避免 Client 超时等待。
void NamedPipeServer::serverLoop() {
    // 服务端循环一次处理一个客户端连接：读命令、执行、写响应，然后重新创建下一条管道实例。
    // 预创建第一个管道实例，确保在进入循环之前管道名称已存在
    HANDLE pipe = CreateNamedPipeA(
        kPipeName,
        // 双向管道：服务端既读取命令，也写回响应。
        PIPE_ACCESS_DUPLEX,
        // 消息模式保证一次消息边界可被客户端按协议识别。
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        kBufferSize,
        kBufferSize,
        0,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        // 管道创建失败时无法服务 CLIENT，线程直接退出。
        return;
    }

    while (running_.load()) {
        // 阻塞等待客户端连接
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            // 连接失败时丢弃当前句柄，并尝试创建新的管道实例。
            CloseHandle(pipe);
            pipe = CreateNamedPipeA(kPipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kBufferSize, kBufferSize, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                // 第一次重建失败时短暂等待，避免忙等。
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
            // stop() 唤醒 ConnectNamedPipe 后会走到这里，立即清理并退出。
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        // 读取客户端发来的命令字符串
        const auto command = readMessage(pipe);
        if (!command.empty()) {
            // 调用 Kernel 命令处理器执行命令，获取响应；共享状态仍由 Kernel 的锁和 worker 机制保护。
            const auto response = handler_(command);

            // 将响应写回客户端（如果写失败，客户端会检测到断连）
            [[maybe_unused]] const bool writeOk = writeMessage(pipe, response);
            // 刷新管道缓冲区，确保响应尽快送达 CLIENT。
            FlushFileBuffers(pipe);
        }

        // 【关键】在关闭当前管道之前预创建下一个管道实例
        // 这样任何时候管道名称都存在，Client 的 WaitNamedPipeA 不会超时
        HANDLE nextPipe = CreateNamedPipeA(
            kPipeName,
            // 下一条管道沿用相同的双向消息模式和缓冲区大小。
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
            // 重试可以缓解短时间资源占用导致的 CreateNamedPipeA 失败。
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
        // 长度读取失败或长度为 0 都视为无效消息。
        return {};
    }

    // 读取指定长度的消息体
    // 使用长度前缀协议而不是固定缓冲区，避免长命令被截断。
    std::vector<char> buffer(length);
    DWORD totalRead = 0;
    while (totalRead < length) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, buffer.data() + totalRead, length - totalRead, &chunk, nullptr) ||
            chunk == 0) {
            // 任一分片读取失败都放弃整条消息。
            return {};
        }
        // 累计已读取字节数，直到达到长度前缀指定的长度。
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
        // 长度前缀写失败时，客户端无法正确读取响应。
        return false;
    }

    // 写入消息体
    // 响应可能很长，循环写完全部字节后再 FlushFileBuffers。
    DWORD totalWritten = 0;
    const char* data = message.data();
    while (totalWritten < length) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, data + totalWritten, length - totalWritten, &chunk, nullptr)) {
            return false;
        }
        // 累计已写字节数，直到整条响应写完。
        totalWritten += chunk;
    }

    return true;
}

} // namespace oscore
