#pragma once

#include <string>
#include <windows.h>

namespace oscore {

// 多实例角色枚举
// MASTER — 第一个启动的实例，持有内核状态并维护 Named Pipe Server
// CLIENT — 后续实例，通过 Named Pipe 向 Master 转发命令
enum class InstanceRole {
    MASTER,
    CLIENT
};

// InstanceGuard —— 通过 Windows Named Mutex 实现跨进程选主
//
// 使用 CreateMutexA 创建命名互斥量。
// 如果互斥量已存在（ERROR_ALREADY_EXISTS），则当前实例为 CLIENT；
// 否则当前实例为 MASTER。
//
// 互斥量名称：Local\OS_SIM_KERNEL_MASTER_MUTEX_2026
// 使用 "Local\" 前缀确保只在当前终端会话内可见，避免跨会话冲突。
class InstanceGuard {
public:
    InstanceGuard() = default;
    ~InstanceGuard();

    InstanceGuard(const InstanceGuard&) = delete;
    InstanceGuard& operator=(const InstanceGuard&) = delete;

    // 初始化：创建或打开命名互斥量，判定角色。
    // 返回 true 表示成功；返回 false 表示 Windows API 调用失败。
    [[nodiscard]] bool initialize();

    // 当前实例的角色
    [[nodiscard]] InstanceRole role() const;

    // 返回 "MASTER" 或 "CLIENT"，用于日志和状态显示
    [[nodiscard]] std::string roleName() const;

private:
    HANDLE mutexHandle_ = nullptr;
    InstanceRole role_ = InstanceRole::CLIENT;
};

} // namespace oscore
