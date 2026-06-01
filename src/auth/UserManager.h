#pragma once

#include "auth/UserAccount.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oscore {

class UserManager {
public:
    bool registerUser(const std::string& username, const std::string& password, std::string& message);
    bool login(const std::string& username, const std::string& password, std::string& message);
    bool logout(std::string& message);

    [[nodiscard]] bool isLoggedIn() const;
    [[nodiscard]] std::string currentUser() const;
    [[nodiscard]] std::string whoami() const;
    [[nodiscard]] bool userExists(const std::string& username) const;
    void clearCurrentSession();

    [[nodiscard]] std::vector<UserAccount> exportUsers() const;
    void importUsers(const std::vector<UserAccount>& users);

private:
    [[nodiscard]] bool validateUsername(const std::string& username, std::string& message) const;
    [[nodiscard]] bool validatePassword(const std::string& password, std::string& message) const;
    [[nodiscard]] std::string makeSalt();
    [[nodiscard]] std::string hashPassword(const std::string& salt, const std::string& password) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, UserAccount> users_;
    std::optional<std::string> currentUser_;
    unsigned long long nextSaltId_ = 1;
};

} // namespace oscore
