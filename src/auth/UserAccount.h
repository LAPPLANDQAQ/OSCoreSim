#pragma once

#include <string>

namespace oscore {

enum class AccountStatus {
    NORMAL,
    LOCKED
};

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
