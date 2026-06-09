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
        message = "[失败] 注册失败：用户名已存在。";
        return false;
    }

    UserAccount account;
    account.username = username;
    account.salt = makeSalt();
    account.passwordHash = hashPassword(account.salt, password);

    users_.emplace(username, std::move(account));
    message = "[成功] 注册成功：用户 '" + username + "' 已创建。";
    return true;
}

bool UserManager::login(const std::string& username, const std::string& password, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentUser_.has_value()) {
        message = "[失败] 登录失败：用户 '" + *currentUser_ + "' 已登录，请先退出。";
        return false;
    }

    auto it = users_.find(username);
    if (it == users_.end()) {
        message = "[失败] 登录失败：用户不存在。";
        return false;
    }

    auto& account = it->second;
    if (account.status == AccountStatus::LOCKED) {
        message = "[失败] 登录失败：账户已被锁定。";
        return false;
    }

    if (account.passwordHash != hashPassword(account.salt, password)) {
        ++account.failedAttempts;
        if (account.failedAttempts >= 3) {
            account.status = AccountStatus::LOCKED;
            message = "[失败] 登录失败：密码错误。连续 3 次失败，账户已被锁定。";
            return false;
        }

        std::ostringstream output;
        output << "[失败] 登录失败：密码错误。已失败 "
               << account.failedAttempts << "/3 次。";
        message = output.str();
        return false;
    }

    account.failedAttempts = 0;
    currentUser_ = username;
    message = "[成功] 登录成功：当前用户为 '" + username + "'。";
    return true;
}

bool UserManager::logout(std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!currentUser_.has_value()) {
        message = "[提示] 退出失败：当前没有登录的用户。";
        return false;
    }

    message = "[成功] 退出成功：用户 '" + *currentUser_ + "' 已退出登录。";
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
    if (currentUser_.has_value()) {
        return "[提示] 当前用户：" + *currentUser_;
    }
    return "[提示] 未登录。";
}

bool UserManager::userExists(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return users_.find(username) != users_.end();
}

void UserManager::clearCurrentSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    currentUser_.reset();
}

bool UserManager::restoreSessionIfUserExists(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!username.empty() && users_.find(username) != users_.end()) {
        currentUser_ = username;
        return true;
    }

    currentUser_.reset();
    return false;
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

    currentUser_.reset();
}

bool UserManager::validateUsername(const std::string& username, std::string& message) const {
    if (username.empty()) {
        message = "[失败] 注册失败：用户名不能为空。";
        return false;
    }

    if (username.size() > 32) {
        message = "[失败] 注册失败：用户名长度须为 1-32 个字符。";
        return false;
    }

    const auto valid = std::all_of(username.begin(), username.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
    });
    if (!valid) {
        message = "[失败] 注册失败：用户名只能包含字母、数字、下划线或连字符。";
        return false;
    }

    return true;
}

bool UserManager::validatePassword(const std::string& password, std::string& message) const {
    if (password.empty()) {
        message = "[失败] 注册失败：密码不能为空。";
        return false;
    }

    if (password.size() > 64) {
        message = "[失败] 注册失败：密码长度须为 1-64 个字符。";
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
