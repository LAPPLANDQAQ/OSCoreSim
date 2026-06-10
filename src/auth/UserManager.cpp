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
    // 注册前先做格式校验；任一校验失败都会写入 message 并停止。
    if (!validateUsername(username, message) || !validatePassword(password, message)) {
        return false;
    }

    // 账户表是共享状态，进入临界区后再检查用户名是否重复。
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_.find(username) != users_.end()) {
        message = "[失败] 注册失败：用户名已存在。";
        return false;
    }

    UserAccount account;
    // 用户名原样保存，后续登录和快照都以它作为唯一键。
    account.username = username;
    // 每个账户生成独立盐值，相同密码也会得到不同散列。
    account.salt = makeSalt();
    // 只保存 salt+password 的散列，不保存明文密码。
    account.passwordHash = hashPassword(account.salt, password);

    // emplace 将账户写入用户名索引表。
    users_.emplace(username, std::move(account));
    message = "[成功] 注册成功：用户 '" + username + "' 已创建。";
    return true;
}

bool UserManager::login(const std::string& username, const std::string& password, std::string& message) {
    // 登录会读写 currentUser_、failedAttempts 和 status，必须整体持锁。
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentUser_.has_value()) {
        message = "[失败] 登录失败：用户 '" + *currentUser_ + "' 已登录，请先退出。";
        return false;
    }

    auto it = users_.find(username);
    if (it == users_.end()) {
        // 用户不存在时不透露更多账户信息，也不修改任何失败计数。
        message = "[失败] 登录失败：用户不存在。";
        return false;
    }

    auto& account = it->second;
    if (account.status == AccountStatus::LOCKED) {
        // 锁定账户即使密码正确也不能登录。
        message = "[失败] 登录失败：账户已被锁定。";
        return false;
    }

    // 使用账户自己的 salt 重新计算散列，与注册时保存的 passwordHash 比较。
    if (account.passwordHash != hashPassword(account.salt, password)) {
        // 密码错误时递增连续失败次数。
        ++account.failedAttempts;
        if (account.failedAttempts >= 3) {
            // 连续 3 次失败后把状态改为 LOCKED，并随快照持久化。
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

    // 登录成功会清零失败计数，重新开始统计连续失败次数。
    account.failedAttempts = 0;
    // currentUser_ 表示当前控制台会话绑定的用户。
    currentUser_ = username;
    message = "[成功] 登录成功：当前用户为 '" + username + "'。";
    return true;
}

bool UserManager::logout(std::string& message) {
    // 登出只需要修改 currentUser_，仍然通过 mutex_ 保护。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!currentUser_.has_value()) {
        message = "[提示] 退出失败：当前没有登录的用户。";
        return false;
    }

    message = "[成功] 退出成功：用户 '" + *currentUser_ + "' 已退出登录。";
    // reset 后 optional 为空，表示无用户登录。
    currentUser_.reset();
    return true;
}

bool UserManager::isLoggedIn() const {
    // 只读查询也持锁，避免与 login/logout 并发读写 currentUser_。
    std::lock_guard<std::mutex> lock(mutex_);
    return currentUser_.has_value();
}

std::string UserManager::currentUser() const {
    // 未登录时返回空字符串，方便 CommandContext 和 Kernel 判断。
    std::lock_guard<std::mutex> lock(mutex_);
    return currentUser_.value_or("");
}

std::string UserManager::whoami() const {
    // whoami 只生成友好提示，不改变会话状态。
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentUser_.has_value()) {
        return "[提示] 当前用户：" + *currentUser_;
    }
    return "[提示] 未登录。";
}

bool UserManager::userExists(const std::string& username) const {
    // Kernel 校验快照或恢复会话时可用此函数确认用户存在。
    std::lock_guard<std::mutex> lock(mutex_);
    return users_.find(username) != users_.end();
}

void UserManager::clearCurrentSession() {
    // 快照加载或系统重置后清除当前会话，避免登录态指向不存在的用户。
    std::lock_guard<std::mutex> lock(mutex_);
    currentUser_.reset();
}

bool UserManager::restoreSessionIfUserExists(const std::string& username) {
    // 手动 load 时尝试保留原登录用户，但只有快照中仍存在该用户才恢复。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!username.empty() && users_.find(username) != users_.end()) {
        currentUser_ = username;
        return true;
    }

    // 用户不存在或传入空用户名时，恢复失败并清空会话。
    currentUser_.reset();
    return false;
}

std::vector<UserAccount> UserManager::exportUsers() const {
    // 导出账户表供 SnapshotStore 序列化，必须在锁内复制出稳定快照。
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UserAccount> users;
    // reserve 避免复制账户时多次扩容。
    users.reserve(users_.size());
    for (const auto& [_, account] : users_) {
        // 复制完整账户记录，包括 salt、hash、失败次数和锁定状态。
        users.push_back(account);
    }

    // unordered_map 遍历顺序不稳定，排序后可让快照内容更可预测。
    std::sort(users.begin(), users.end(), [](const UserAccount& left, const UserAccount& right) {
        return left.username < right.username;
    });
    return users;
}

void UserManager::importUsers(const std::vector<UserAccount>& users) {
    // 导入会整体替换账户表，必须在锁内完成。
    std::lock_guard<std::mutex> lock(mutex_);
    users_.clear();
    for (const auto& account : users) {
        // 使用用户名作为 key，保留快照中的账户状态。
        users_[account.username] = account;
    }

    // 导入账户后默认清除会话；是否恢复由 Kernel::restoreSessionIfUserExists 决定。
    currentUser_.reset();
}

bool UserManager::validateUsername(const std::string& username, std::string& message) const {
    // 用户名不能为空，否则无法作为 users_ 的稳定键。
    if (username.empty()) {
        message = "[失败] 注册失败：用户名不能为空。";
        return false;
    }

    // 长度限制为 32，兼顾课程演示表格显示和输入简洁性。
    if (username.size() > 32) {
        message = "[失败] 注册失败：用户名长度须为 1-32 个字符。";
        return false;
    }

    // 用户名只允许 ASCII 字母、数字、下划线和连字符，避免命令参数解析歧义。
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
    // 密码不能为空，避免产生弱账户。
    if (password.empty()) {
        message = "[失败] 注册失败：密码不能为空。";
        return false;
    }

    // 长度上限防止异常长输入影响命令行演示和快照体积。
    if (password.size() > 64) {
        message = "[失败] 注册失败：密码长度须为 1-64 个字符。";
        return false;
    }

    return true;
}

std::string UserManager::makeSalt() {
    // steady_clock 计数提供时间变化量，nextSaltId_ 确保同一时刻连续注册也不同。
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream output;
    output << "salt_" << now << '_' << nextSaltId_++;
    return output.str();
}

std::string UserManager::hashPassword(const std::string& salt, const std::string& password) const {
    // FNV-1a 的 64 位 offset basis。
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    // FNV-1a 的 64 位 prime。
    constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t hash = offsetBasis;
    // 把 salt 拼在密码前面，使相同密码在不同账户中散列不同。
    const std::string input = salt + password;
    for (unsigned char ch : input) {
        // FNV-1a：先异或当前字节。
        hash ^= ch;
        // 再乘以固定 prime，形成滚动散列。
        hash *= prime;
    }

    std::ostringstream output;
    // 输出固定宽度 16 位十六进制字符串，便于二进制快照保存和比对。
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

} // namespace oscore
