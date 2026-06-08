#pragma once

#include <string>

namespace oscore {

enum class AccountStatus {
    NORMAL,
    LOCKED
};

// UserAccount 是可持久化账户记录，只保存盐和密码散列，不保存明文密码。
// failedAttempts 用于演示三次登录失败后锁定账户的安全策略。
struct UserAccount {
    std::string username;
    std::string passwordHash;
    std::string salt;
    int failedAttempts = 0;
    AccountStatus status = AccountStatus::NORMAL;
};

[[nodiscard]] inline const char* toString(AccountStatus status) {
    switch (status) {
    case AccountStatus::NORMAL:
        return "NORMAL";
    case AccountStatus::LOCKED:
        return "LOCKED";
    }
    return "UNKNOWN";
}

} // namespace oscore
