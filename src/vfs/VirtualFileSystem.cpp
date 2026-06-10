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
    // VFS 操作必须绑定当前登录用户。
    if (owner.empty()) {
        message = "[失败] 创建失败：请先登录。";
        return false;
    }

    std::string validateMessage;
    // 文件名合法性统一由 isValidFileName 检查。
    if (!isValidFileName(name, validateMessage)) {
        message = validateMessage;
        return false;
    }

    if (findFileIndex(owner, name) >= 0) {
        // 同一用户下不允许重名；不同用户可拥有同名文件。
        message = "[失败] 创建失败：文件 '" + name + "' 已存在。";
        return false;
    }

    // 记录创建和修改时间，创建时二者相同。
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    VirtualFile file;
    // fileId 全局递增，不因删除文件而复用。
    file.fileId = nextFileId_++;
    file.owner = owner;
    file.name = name;
    file.content.clear();
    file.createdAt = now;
    file.modifiedAt = now;
    // 文件内容和元数据都保存在内存中的 files_ 向量。
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
    // 写文件也要求当前用户身份，用 owner 隔离不同用户文件。
    if (owner.empty()) {
        message = "[失败] 写入失败：请先登录。";
        return false;
    }

    std::string validateMessage;
    // 写入不存在文件时也需要先校验文件名，因为后续可能自动创建。
    if (!isValidFileName(name, validateMessage)) {
        message = validateMessage;
        return false;
    }

    const auto index = findFileIndex(owner, name);
    if (index >= 0) {
        // 文件已存在时覆盖全部内容，不做追加。
        files_[index].content = content;
        // 修改内容后刷新 modifiedAt，createdAt 保持不变。
        files_[index].modifiedAt = static_cast<std::uint64_t>(std::time(nullptr));

        std::ostringstream output;
        output << "[成功] 虚拟文件已写入。\n"
               << "名称: " << name << '\n'
               << "大小: " << content.size() << " 字节";
        message = output.str();
        return true;
    }

    // 文件不存在时执行写时创建，便于一次命令完成演示。
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    VirtualFile file;
    // 自动创建的新文件同样分配全局 fileId。
    file.fileId = nextFileId_++;
    file.owner = owner;
    file.name = name;
    file.content = content;
    file.createdAt = now;
    file.modifiedAt = now;
    // 多行内容已经由 Kernel 解码为真实换行，直接存入 content。
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
    // 读取需要当前用户，防止跨用户读取文件内容。
    if (owner.empty()) {
        return "[失败] 读取失败：请先登录。";
    }

    const auto index = findFileIndex(owner, name);
    if (index < 0) {
        // 查找时同时匹配 owner 和 name。
        return "[失败] 读取失败：文件 '" + name + "' 不存在。";
    }

    const auto& file = files_[index];
    std::ostringstream output;
    // 输出文件元数据和内容，内容区域用分隔线包围。
    output << "=== 文件: " << file.name << " ===\n"
           << "所有者: " << file.owner << '\n'
           << "大小: " << file.content.size() << " 字节\n"
           << "内容:\n"
           << "--------------------------------\n";
    if (file.content.empty()) {
        // 空文件用占位文本显示，避免看起来像命令没有输出。
        output << "(空)\n";
    } else {
        // content 可能包含多行文本，按原始内容输出。
        output << file.content;
        if (!file.content.empty() && file.content.back() != '\n') {
            // 如果内容本身没有以换行结尾，补一个换行让下方分隔线单独成行。
            output << '\n';
        }
    }
    output << "--------------------------------";
    return output.str();
}

bool VirtualFileSystem::deleteFile(const std::string& owner,
                                   const std::string& name,
                                   std::string& message) {
    // 删除只允许删除当前用户自己的文件。
    if (owner.empty()) {
        message = "[失败] 删除失败：请先登录。";
        return false;
    }

    const auto index = findFileIndex(owner, name);
    if (index < 0) {
        // 不存在或属于其他用户都表现为当前用户下不存在。
        message = "[失败] 删除失败：文件 '" + name + "' 不存在。";
        return false;
    }

    // vector erase 删除文件记录；文件 ID 不回收。
    files_.erase(files_.begin() + index);
    message = "[成功] 虚拟文件已删除: " + name;
    return true;
}

std::string VirtualFileSystem::listFiles(const std::string& owner) const {
    // ls_file 按当前用户过滤文件列表。
    if (owner.empty()) {
        return "[失败] 列表失败：请先登录。";
    }

    std::vector<VirtualFile> userFiles;
    for (const auto& file : files_) {
        if (file.owner == owner) {
            // 复制当前用户文件，避免排序影响原始 files_ 顺序。
            userFiles.push_back(file);
        }
    }
    // 按 fileId 排序，显示顺序与创建顺序一致。
    std::sort(userFiles.begin(), userFiles.end(), [](const VirtualFile& a, const VirtualFile& b) {
        return a.fileId < b.fileId;
    });

    if (userFiles.empty()) {
        return "=== 虚拟文件 [用户: " + owner + "] ===\n"
               "暂无虚拟文件。";
    }

    std::ostringstream output;
    // 表格列展示 ID、文件名、内容字节数和修改时间戳。
    output << "=== 虚拟文件 [用户: " << owner << "] ===\n"
           << std::left
           << padRightDisplayWidth("ID", 6)
           << padRightDisplayWidth("Name", 14)
           << padRightDisplayWidth("Size", 8)
           << "Modified\n";

    for (const auto& file : userFiles) {
        // content.size() 是 UTF-8 字节数，不是中文字符数。
        output << std::left
               << padRightDisplayWidth(std::to_string(file.fileId), 6)
               << padRightDisplayWidth(file.name, 14)
               << padRightDisplayWidth(std::to_string(file.content.size()), 8)
               << file.modifiedAt << '\n';
    }

    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        // 去掉最后一个换行，让命令输出更紧凑。
        result.pop_back();
    }
    return result;
}

std::size_t VirtualFileSystem::fileCountForUser(const std::string& owner) const {
    // overview 使用该统计展示当前用户 VFS 文件数量。
    return static_cast<std::size_t>(std::count_if(files_.begin(), files_.end(),
        [&owner](const VirtualFile& f) { return f.owner == owner; }));
}

std::vector<VirtualFile> VirtualFileSystem::exportFiles() const {
    // 导出所有用户的文件供快照保存。
    auto result = files_;
    // 按 fileId 排序，保证持久化写入顺序稳定。
    std::sort(result.begin(), result.end(), [](const VirtualFile& a, const VirtualFile& b) {
        return a.fileId < b.fileId;
    });
    return result;
}

void VirtualFileSystem::importFiles(const std::vector<VirtualFile>& files) {
    // 快照加载时整体替换 VFS 文件表。
    files_ = files;
}

std::uint32_t VirtualFileSystem::exportNextFileId() const {
    // 持久化 nextFileId_，避免加载后新文件 ID 与旧文件冲突。
    return nextFileId_;
}

void VirtualFileSystem::importNextFileId(std::uint32_t nextFileId) {
    // nextFileId 至少为 1，fileId=0 保留为无效默认值。
    nextFileId_ = std::max<std::uint32_t>(nextFileId, 1);
}

int VirtualFileSystem::findFileIndex(const std::string& owner,
                                     const std::string& name) const {
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i].owner == owner && files_[i].name == name) {
            // 返回 vector 下标，调用方用于读取、覆盖或 erase。
            return static_cast<int>(i);
        }
    }
    // -1 表示当前用户下没有该文件。
    return -1;
}

bool VirtualFileSystem::isValidFileName(const std::string& name, std::string& message) {
    // 文件名不能为空，否则无法作为 owner+name 查找键。
    if (name.empty()) {
        message = "[失败] 文件名不能为空。";
        return false;
    }
    // 按 UTF-8 字节数限制长度，中文文件名可用但会占多个字节。
    if (name.size() > 128) {
        message = "[失败] 文件名长度必须为 1-128 字节 (UTF-8)。";
        return false;
    }
    for (const auto ch : name) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc <= 0x1F || uc == 0x7F) {
            // 控制字符不可见，可能破坏终端显示和快照可读性。
            message = "[失败] 文件名包含控制字符。";
            return false;
        }
        switch (uc) {
        case '/': case '\\': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|':
            // 禁止常见路径或 Windows 文件名特殊字符，避免和真实路径语义混淆。
            message = "[失败] 文件名包含禁止字符: '"
                      + std::string(1, ch) + "'。";
            return false;
        default: break;
        }
    }
    return true;
}

} // namespace oscore
