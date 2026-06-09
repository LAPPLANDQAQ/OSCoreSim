#include "vfs/VirtualFileSystem.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace oscore {

// 虚拟文件系统只保存在内存中的 VirtualFile 表；owner 是隔离边界，不同用户可拥有同名文件。
bool VirtualFileSystem::createFile(const std::string& owner,
                                   const std::string& name,
                                   std::string& message) {
    if (owner.empty()) {
        message = "VFS create failed: user must login first.";
        return false;
    }

    std::string validateMessage;
    if (!isValidFileName(name, validateMessage)) {
        message = validateMessage;
        return false;
    }

    // 检查同一用户下是否有同名文件
    if (findFileIndex(owner, name) >= 0) {
        message = "VFS create failed: file '" + name + "' already exists.";
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
    output << "[OK] Virtual file created.\n"
           << "Name: " << name << '\n'
           << "Owner: " << owner;
    message = output.str();
    return true;
}

bool VirtualFileSystem::writeFile(const std::string& owner,
                                  const std::string& name,
                                  const std::string& content,
                                  std::string& message) {
    if (owner.empty()) {
        message = "VFS write failed: user must login first.";
        return false;
    }

    std::string validateMessage;
    if (!isValidFileName(name, validateMessage)) {
        message = validateMessage;
        return false;
    }

    // 查找已有文件
    const auto index = findFileIndex(owner, name);
    if (index >= 0) {
        // 更新已有文件
        files_[index].content = content;
        files_[index].modifiedAt = static_cast<std::uint64_t>(std::time(nullptr));

        std::ostringstream output;
        output << "[OK] Virtual file written.\n"
               << "Name: " << name << '\n'
               << "Size: " << content.size() << " bytes";
        message = output.str();
        return true;
    }

    // 文件不存在 → 自动创建（方便课程演示）
    // write_file 采用“有则覆盖、无则创建”，让脚本演示不必先 touch_file。
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
    output << "[OK] Virtual file auto-created and written.\n"
           << "Name: " << name << '\n'
           << "Size: " << content.size() << " bytes";
    message = output.str();
    return true;
}

std::string VirtualFileSystem::readFile(const std::string& owner,
                                        const std::string& name) const {
    if (owner.empty()) {
        return "VFS read failed: user must login first.";
    }

    // 通过 owner + name 查找，保证用户只能读取自己的虚拟文件。
    const auto index = findFileIndex(owner, name);
    if (index < 0) {
        return "VFS read failed: file '" + name + "' does not exist.";
    }

    const auto& file = files_[index];
    std::ostringstream output;
    output << "=== Virtual File ===\n"
           << "Name: " << file.name << '\n'
           << "Owner: " << file.owner << '\n'
           << "Size: " << file.content.size() << " bytes\n"
           << "Content:\n"
           << file.content;
    return output.str();
}

bool VirtualFileSystem::deleteFile(const std::string& owner,
                                   const std::string& name,
                                   std::string& message) {
    if (owner.empty()) {
        message = "VFS delete failed: user must login first.";
        return false;
    }

    // 删除同样按 owner 隔离，避免不同用户同名文件互相影响。
    const auto index = findFileIndex(owner, name);
    if (index < 0) {
        message = "VFS delete failed: file '" + name + "' does not exist.";
        return false;
    }

    files_.erase(files_.begin() + index);
    message = "[OK] Virtual file removed: " + name;
    return true;
}

std::string VirtualFileSystem::listFiles(const std::string& owner) const {
    if (owner.empty()) {
        return "VFS list failed: user must login first.";
    }

    // 收集当前用户的文件，按 fileId 排序
    // list/read/delete 都只展示当前 owner 的文件，避免跨用户读取虚拟文件内容。
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
        return "=== Virtual Files for user: " + owner + " ===\n"
               "No virtual files found.";
    }

    std::ostringstream output;
    output << "=== Virtual Files for user: " << owner << " ===\n"
           << std::left
           << std::setw(6) << "ID"
           << std::setw(14) << "Name"
           << std::setw(8) << "Size"
           << "Modified\n";

    for (const auto& file : userFiles) {
        output << std::left
               << std::setw(6) << file.fileId
               << std::setw(14) << file.name
               << std::setw(8) << file.content.size()
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
    // 按 fileId 排序后返回副本
    // 快照持久化通过 export/import 复制纯数据，不暴露内部 files_ 容器给 SnapshotStore 修改。
    auto result = files_;
    std::sort(result.begin(), result.end(), [](const VirtualFile& a, const VirtualFile& b) {
        return a.fileId < b.fileId;
    });
    return result;
}

void VirtualFileSystem::importFiles(const std::vector<VirtualFile>& files) {
    // load 快照时整体替换 VFS 表；nextFileId 由独立字段恢复，避免新文件 ID 回退。
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
    // owner + name 是逻辑唯一键，支持不同用户创建同名文件。
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i].owner == owner && files_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool VirtualFileSystem::isValidFileName(const std::string& name, std::string& message) {
    if (name.empty()) {
        message = "VFS failed: file name cannot be empty.";
        return false;
    }
    if (name.size() > 64) {
        message = "VFS failed: file name must be 1 to 64 characters.";
        return false;
    }
    // 允许字母、数字、下划线、横线、点号
    const auto valid = std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
    if (!valid) {
        message = "VFS failed: file name may only contain letters, digits, underscore, hyphen, or dot.";
        return false;
    }
    return true;
}

} // namespace oscore
