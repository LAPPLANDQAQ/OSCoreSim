#include "vfs/VirtualFileSystem.h"

#include "util/StringUtil.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace oscore {

// createFile：创建空虚拟文件。
// 校验：用户已登录 → 文件名合法（isValidFileName） → 同一用户下无重名文件。
// 为新文件分配全局唯一 fileId，记录创建/修改时间戳（Unix timestamp）。
bool VirtualFileSystem::createFile(const std::string& owner,
                                   const std::string& name,
                                   std::string& message) {
    if (owner.empty()) {
        message = "[失败] 创建失败：请先登录。";
        return false;
    }

    std::string validateMessage;
    if (!isValidFileName(name, validateMessage)) {
        message = validateMessage;
        return false;
    }

    if (findFileIndex(owner, name) >= 0) {
        message = "[失败] 创建失败：文件 '" + name + "' 已存在。";
        return false;
    }

    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    VirtualFile file;
    file.fileId = nextFileId_++;
    file.owner = owner;
    file.name = name;
    file.content.clear();
    file.createdAt = now;
    file.modifiedAt = now;
    files_.push_back(std::move(file));

    std::ostringstream output;
    output << "[成功] 虚拟文件已创建。\n"
           << "名称: " << name << '\n'
           << "所有者: " << owner;
    message = output.str();
    return true;
}

// writeFile：写入虚拟文件内容。
// 写时创建语义：如果文件已存在则覆盖内容并更新修改时间，如果不存在则自动创建文件后再写入。
// 这避免了课程演示中必须先 touch_file 再 write_file 的繁琐流程。
bool VirtualFileSystem::writeFile(const std::string& owner,
                                  const std::string& name,
                                  const std::string& content,
                                  std::string& message) {
    if (owner.empty()) {
        message = "[失败] 写入失败：请先登录。";
        return false;
    }

    std::string validateMessage;
    if (!isValidFileName(name, validateMessage)) {
        message = validateMessage;
        return false;
    }

    const auto index = findFileIndex(owner, name);
    if (index >= 0) {
        files_[index].content = content;
        files_[index].modifiedAt = static_cast<std::uint64_t>(std::time(nullptr));

        std::ostringstream output;
        output << "[成功] 虚拟文件已写入。\n"
               << "名称: " << name << '\n'
               << "大小: " << content.size() << " 字节";
        message = output.str();
        return true;
    }

    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    VirtualFile file;
    file.fileId = nextFileId_++;
    file.owner = owner;
    file.name = name;
    file.content = content;
    file.createdAt = now;
    file.modifiedAt = now;
    files_.push_back(std::move(file));

    std::ostringstream output;
    output << "[成功] 虚拟文件已自动创建并写入。\n"
           << "名称: " << name << '\n'
           << "大小: " << content.size() << " 字节";
    message = output.str();
    return true;
}

std::string VirtualFileSystem::readFile(const std::string& owner,
                                        const std::string& name) const {
    if (owner.empty()) {
        return "[失败] 读取失败：请先登录。";
    }

    const auto index = findFileIndex(owner, name);
    if (index < 0) {
        return "[失败] 读取失败：文件 '" + name + "' 不存在。";
    }

    const auto& file = files_[index];
    std::ostringstream output;
    output << "=== 文件: " << file.name << " ===\n"
           << "所有者: " << file.owner << '\n'
           << "大小: " << file.content.size() << " 字节\n"
           << "内容:\n"
           << "--------------------------------\n";
    if (file.content.empty()) {
        output << "(空)\n";
    } else {
        output << file.content;
        if (!file.content.empty() && file.content.back() != '\n') {
            output << '\n';
        }
    }
    output << "--------------------------------";
    return output.str();
}

bool VirtualFileSystem::deleteFile(const std::string& owner,
                                   const std::string& name,
                                   std::string& message) {
    if (owner.empty()) {
        message = "[失败] 删除失败：请先登录。";
        return false;
    }

    const auto index = findFileIndex(owner, name);
    if (index < 0) {
        message = "[失败] 删除失败：文件 '" + name + "' 不存在。";
        return false;
    }

    files_.erase(files_.begin() + index);
    message = "[成功] 虚拟文件已删除: " + name;
    return true;
}

std::string VirtualFileSystem::listFiles(const std::string& owner) const {
    if (owner.empty()) {
        return "[失败] 列表失败：请先登录。";
    }

    std::vector<VirtualFile> userFiles;
    for (const auto& file : files_) {
        if (file.owner == owner) {
            userFiles.push_back(file);
        }
    }
    std::sort(userFiles.begin(), userFiles.end(), [](const VirtualFile& a, const VirtualFile& b) {
        return a.fileId < b.fileId;
    });

    if (userFiles.empty()) {
        return "=== 虚拟文件 [用户: " + owner + "] ===\n"
               "暂无虚拟文件。";
    }

    std::ostringstream output;
    output << "=== 虚拟文件 [用户: " << owner << "] ===\n"
           << std::left
           << padRightDisplayWidth("ID", 6)
           << padRightDisplayWidth("Name", 14)
           << padRightDisplayWidth("Size", 8)
           << "Modified\n";

    for (const auto& file : userFiles) {
        output << std::left
               << padRightDisplayWidth(std::to_string(file.fileId), 6)
               << padRightDisplayWidth(file.name, 14)
               << padRightDisplayWidth(std::to_string(file.content.size()), 8)
               << file.modifiedAt << '\n';
    }

    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

std::size_t VirtualFileSystem::fileCountForUser(const std::string& owner) const {
    return static_cast<std::size_t>(std::count_if(files_.begin(), files_.end(),
        [&owner](const VirtualFile& f) { return f.owner == owner; }));
}

std::vector<VirtualFile> VirtualFileSystem::exportFiles() const {
    auto result = files_;
    std::sort(result.begin(), result.end(), [](const VirtualFile& a, const VirtualFile& b) {
        return a.fileId < b.fileId;
    });
    return result;
}

void VirtualFileSystem::importFiles(const std::vector<VirtualFile>& files) {
    files_ = files;
}

std::uint32_t VirtualFileSystem::exportNextFileId() const {
    return nextFileId_;
}

void VirtualFileSystem::importNextFileId(std::uint32_t nextFileId) {
    nextFileId_ = std::max<std::uint32_t>(nextFileId, 1);
}

int VirtualFileSystem::findFileIndex(const std::string& owner,
                                     const std::string& name) const {
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i].owner == owner && files_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool VirtualFileSystem::isValidFileName(const std::string& name, std::string& message) {
    if (name.empty()) {
        message = "[失败] 文件名不能为空。";
        return false;
    }
    if (name.size() > 128) {
        message = "[失败] 文件名长度必须为 1-128 字节 (UTF-8)。";
        return false;
    }
    for (const auto ch : name) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc <= 0x1F || uc == 0x7F) {
            message = "[失败] 文件名包含控制字符。";
            return false;
        }
        switch (uc) {
        case '/': case '\\': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|':
            message = "[失败] 文件名包含禁止字符: '"
                      + std::string(1, ch) + "'。";
            return false;
        default: break;
        }
    }
    return true;
}

} // namespace oscore
