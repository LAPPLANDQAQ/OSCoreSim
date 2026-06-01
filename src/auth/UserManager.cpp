#include "auth/UserManager.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>

namespace oscore {

bool UserManager::registerUser(const std::string& username, const std::string& password, std::string& message) {
    if (!validateUsername(username, message) || !validatePassword(password, message)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (users_.find(username) != users_.end()) {
        message = "Register failed: username already exists.";
        return false;
    }

    UserAccount account;
    account.username = username;
    account.salt = makeSalt();
    account.passwordHash = hashPassword(account.salt, password);

    users_.emplace(username, std::move(account));
    message = "Register success: user '" + username + "' created.";
    return true;
}

bool UserManager::login(const std::string& username, const std::string& password, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentUser_.has_value()) {
        message = "Login failed: user '" + *currentUser_ + "' is already logged in.";
        return false;
    }

    auto it = users_.find(username);
    if (it == users_.end()) {
        message = "Login failed: user does not exist.";
        return false;
    }

    auto& account = it->second;
    if (account.status == AccountStatus::LOCKED) {
        message = "Login failed: account is locked.";
        return false;
    }

    if (account.passwordHash != hashPassword(account.salt, password)) {
        ++account.failedAttempts;
        if (account.failedAttempts >= 3) {
            account.status = AccountStatus::LOCKED;
            message = "Login failed: password is wrong. Account is locked after 3 failed attempts.";
            return false;
        }

        std::ostringstream output;
        output << "Login failed: password is wrong. Failed attempts: "
               << account.failedAttempts << "/3.";
        message = output.str();
        return false;
    }

    account.failedAttempts = 0;
    currentUser_ = username;
    message = "Login success: current user is '" + username + "'.";
    return true;
}

bool UserManager::logout(std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!currentUser_.has_value()) {
        message = "Logout skipped: no user is logged in.";
        return false;
    }

    message = "Logout success: user '" + *currentUser_ + "' logged out.";
    currentUser_.reset();
    return true;
}

bool UserManager::isLoggedIn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentUser_.has_value();
}

std::string UserManager::currentUser() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentUser_.value_or("");
}

std::string UserManager::whoami() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentUser_.value_or("not logged in");
}

bool UserManager::userExists(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return users_.find(username) != users_.end();
}

std::vector<UserAccount> UserManager::exportUsers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UserAccount> users;
    users.reserve(users_.size());
    for (const auto& [_, account] : users_) {
        users.push_back(account);
    }

    std::sort(users.begin(), users.end(), [](const UserAccount& left, const UserAccount& right) {
        return left.username < right.username;
    });
    return users;
}

void UserManager::importUsers(const std::vector<UserAccount>& users) {
    std::lock_guard<std::mutex> lock(mutex_);
    users_.clear();
    for (const auto& account : users) {
        users_[account.username] = account;
    }

    // 持久化恢复账户数据时不恢复交互会话，避免重启后自动登录。
    currentUser_.reset();
}

bool UserManager::validateUsername(const std::string& username, std::string& message) const {
    if (username.empty()) {
        message = "Register failed: username cannot be empty.";
        return false;
    }

    if (username.size() > 32) {
        message = "Register failed: username length must be 1 to 32 characters.";
        return false;
    }

    const auto valid = std::all_of(username.begin(), username.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
    });
    if (!valid) {
        message = "Register failed: username may only contain letters, digits, underscore, or hyphen.";
        return false;
    }

    return true;
}

bool UserManager::validatePassword(const std::string& password, std::string& message) const {
    if (password.empty()) {
        message = "Register failed: password cannot be empty.";
        return false;
    }

    if (password.size() > 64) {
        message = "Register failed: password length must be 1 to 64 characters.";
        return false;
    }

    return true;
}

std::string UserManager::makeSalt() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "salt_" << now << '_' << nextSaltId_++;
    return output.str();
}

std::string UserManager::hashPassword(const std::string& salt, const std::string& password) const {
    // 课程设计用途：使用固定 FNV-1a 风格散列避免保存明文密码。
    // 这不是生产级密码学方案，真实系统应使用 bcrypt/Argon2/PBKDF2 等专用算法。
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t hash = offsetBasis;
    const std::string input = salt + password;
    for (unsigned char ch : input) {
        hash ^= ch;
        hash *= prime;
    }

    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

} // namespace oscore
