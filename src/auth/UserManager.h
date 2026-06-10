#pragma once

#include "auth/UserAccount.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oscore {

// UserManager：用户账户管理系统。
//
// 职责：
//   - 用户注册（用户名/密码验证，salt + FNV-1a 散列存储）
//   - 用户登录（密码校验，连续 3 次失败锁定账户）
//   - 用户登出（清除当前会话）
//   - whoami 查询当前登录用户
//   - 持久化导入/导出
//
// 安全机制（课程设计级别，非生产级）：
//   - 密码不保存明文，存储 salt + FNV-1a 风格的 16 进制散列值
//   - 连续 3 次密码错误 → 账户锁定（LOCKED），锁定后正确密码也无法登录
//   - 锁定状态随快照持久化
//
// 线程安全：所有公开方法内部加锁（mutex_）。
class UserManager {
public:
    // 注册新用户：校验格式、生成盐值、保存密码散列。
    bool registerUser(const std::string& username, const std::string& password, std::string& message);
    // 登录用户：校验密码、更新失败计数、必要时锁定账户。
    bool login(const std::string& username, const std::string& password, std::string& message);
    // 退出当前会话：只清除 currentUser_，不删除账户。
    bool logout(std::string& message);

    [[nodiscard]] bool isLoggedIn() const;
    [[nodiscard]] std::string currentUser() const;     // 返回当前用户名，空字符串 = 未登录
    [[nodiscard]] std::string whoami() const;           // 友好的当前用户提示
    [[nodiscard]] bool userExists(const std::string& username) const;

    // 清除当前登录会话（快照加载后调用）
    void clearCurrentSession();
    // 如果用户仍存在则恢复会话（load 命令保持登录时调用）
    bool restoreSessionIfUserExists(const std::string& username);

    // === 持久化接口 ===
    [[nodiscard]] std::vector<UserAccount> exportUsers() const;
    void importUsers(const std::vector<UserAccount>& users);

private:
    // 验证用户名格式：1-32 字符，仅字母/数字/下划线/连字符
    [[nodiscard]] bool validateUsername(const std::string& username, std::string& message) const;
    // 验证密码格式：1-64 字符
    [[nodiscard]] bool validatePassword(const std::string& password, std::string& message) const;

    // 生成随机盐值（时间戳 + 自增 ID）
    [[nodiscard]] std::string makeSalt();
    // FNV-1a 风格密码散列（固定 64 位 offset + prime，输出 16 进制字符串）
    [[nodiscard]] std::string hashPassword(const std::string& salt, const std::string& password) const;

    mutable std::mutex mutex_;                                // 保护 users_、currentUser_ 和 nextSaltId_。
    std::unordered_map<std::string, UserAccount> users_;  // 用户名 → 账户记录
    std::optional<std::string> currentUser_;              // 当前登录用户名（nullopt = 未登录）
    std::uint64_t nextSaltId_ = 1;                        // 盐值自增计数器
};

} // namespace oscore
