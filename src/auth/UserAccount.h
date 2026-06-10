#pragma once

#include <string>

namespace oscore {

// 账户状态枚举。
// NORMAL: 正常状态，可以登录
// LOCKED: 因连续 3 次密码错误锁定，需管理员重置（本课程设计中通过 save/load 持久化锁定状态）
enum class AccountStatus {
    NORMAL,
    LOCKED
};

// UserAccount：可持久化用户账户记录。
// 只保存 salt + FNV-1a 散列值，不保存明文密码。
// failedAttempts 跟踪连续登录失败次数，达到 3 次自动锁定。
struct UserAccount {
    std::string username;                    // 用户名（1-32 字符，字母/数字/下划线/连字符）
    std::string passwordHash;               // salt + password 的 FNV-1a 散列（16 进制字符串）
    std::string salt;                        // 随机盐值，防彩虹表
    int failedAttempts = 0;                 // 当前连续登录失败次数
    AccountStatus status = AccountStatus::NORMAL;  // NORMAL 或 LOCKED
};

[[nodiscard]] inline const char* toString(AccountStatus status) {
    // 将枚举值转换为可持久化、可展示的稳定文本。
    switch (status) {
    case AccountStatus::NORMAL: return "NORMAL";
    case AccountStatus::LOCKED: return "LOCKED";
    }
    // 理论上不会到达；保留 UNKNOWN 便于发现异常枚举值。
    return "UNKNOWN";
}

} // namespace oscore
