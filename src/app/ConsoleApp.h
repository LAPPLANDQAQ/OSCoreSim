#pragma once

#include "ipc/InstanceGuard.h"
#include "ipc/NamedPipeClient.h"
#include "ipc/NamedPipeServer.h"
#include "kernel/Kernel.h"

#include <iosfwd>

namespace oscore {

// ConsoleApp —— 命令行交互前端
//
// 支持两种运行模式：
//   MASTER — 持有 Kernel，启动 Pipe Server，本地输入直接提交 Kernel
//   CLIENT — 不持有 Kernel，通过 NamedPipeClient 将命令转发给 Master
class ConsoleApp {
public:
    ConsoleApp() = default;

    // 以指定角色运行程序。role 由 InstanceGuard 在 main() 中判定后传入。
    int run(InstanceRole role);
    int run(std::istream& input, std::ostream& output, InstanceRole role);

private:
    // Master 模式主循环：本地输入 → Kernel::submitCommand()
    void masterLoop(std::istream& input, std::ostream& output);

    // Client 模式主循环：本地输入 → NamedPipeClient::sendCommand() → 打印响应
    void clientLoop(std::istream& input, std::ostream& output);

    // 判断是否为本地退出命令（Client 下 exit/quit 只关闭自身窗口）
    [[nodiscard]] static bool isLocalExitCommand(const std::string& line);

    Kernel kernel_;
    NamedPipeServer pipeServer_;
    NamedPipeClient pipeClient_;
};

} // namespace oscore
