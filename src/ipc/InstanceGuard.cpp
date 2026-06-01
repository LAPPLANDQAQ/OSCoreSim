#include "ipc/InstanceGuard.h"

namespace oscore {

// 命名互斥量名称 —— 用于同一台机器上多个 os_sim.exe 实例之间判定唯一 MASTER
static constexpr const char* kMasterMutexName = "Local\\OS_SIM_KERNEL_MASTER_MUTEX_2026";

InstanceGuard::~InstanceGuard() {
    if (mutexHandle_ != nullptr) {
        CloseHandle(mutexHandle_);
        mutexHandle_ = nullptr;
    }
}

bool InstanceGuard::initialize() {
    // 创建命名互斥量。如果已存在，GetLastError() 返回 ERROR_ALREADY_EXISTS。
    mutexHandle_ = CreateMutexA(
        nullptr,            // 默认安全属性
        FALSE,              // 不需要立即拥有
        kMasterMutexName    // 全局唯一名称
    );

    if (mutexHandle_ == nullptr) {
        // CreateMutexA 失败（极少情况），默认作为 CLIENT，调用方应检查返回值
        role_ = InstanceRole::CLIENT;
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 互斥量已存在 → 已有 MASTER 在运行，当前实例为 CLIENT
        role_ = InstanceRole::CLIENT;
    } else {
        // 互斥量新创建 → 当前实例为 MASTER
        role_ = InstanceRole::MASTER;
    }

    return true;
}

InstanceRole InstanceGuard::role() const {
    return role_;
}

std::string InstanceGuard::roleName() const {
    return role_ == InstanceRole::MASTER ? "MASTER" : "CLIENT";
}

} // namespace oscore
