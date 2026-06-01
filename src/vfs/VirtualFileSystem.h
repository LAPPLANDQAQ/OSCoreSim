#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace oscore {

// VirtualFile —— 虚拟文件系统中的单个文件
//
// 每个文件归属于一个用户（owner），不同用户可以有同名文件。
// fileId 全局唯一递增。
struct VirtualFile {
    std::uint32_t fileId = 0;
    std::string owner;
    std::string name;
    std::string content;
    std::uint64_t createdAt = 0;
    std::uint64_t modifiedAt = 0;
};

// VirtualFileSystem —— 最小虚拟文件系统
//
// 每个用户拥有自己的虚拟文件集合，用户登录后只能操作自己的文件。
// 所有文件内容存放在内存中，通过 save/load 接入二进制持久化。
//
// 线程安全：外部调用方（Kernel）负责加锁，本类内部不加锁。
class VirtualFileSystem {
public:
    // 创建空虚拟文件，同一用户下文件名不重复
    bool createFile(const std::string& owner,
                    const std::string& name,
                    std::string& message);

    // 写入文件内容；若文件不存在则自动创建（方便课程演示）
    bool writeFile(const std::string& owner,
                   const std::string& name,
                   const std::string& content,
                   std::string& message);

    // 读取文件内容，返回格式化字符串
    [[nodiscard]] std::string readFile(const std::string& owner,
                                       const std::string& name) const;

    // 删除文件
    bool deleteFile(const std::string& owner,
                    const std::string& name,
                    std::string& message);

    // 列出当前用户所有文件
    [[nodiscard]] std::string listFiles(const std::string& owner) const;

    // 获取当前用户的文件数量
    [[nodiscard]] std::size_t fileCountForUser(const std::string& owner) const;

    // === 持久化接口 ===
    [[nodiscard]] std::vector<VirtualFile> exportFiles() const;
    void importFiles(const std::vector<VirtualFile>& files);

    [[nodiscard]] std::uint32_t exportNextFileId() const;
    void importNextFileId(std::uint32_t nextFileId);

private:
    // 按 owner + name 查找文件，返回索引或 -1
    [[nodiscard]] int findFileIndex(const std::string& owner,
                                    const std::string& name) const;

    // 验证文件名合法性
    [[nodiscard]] static bool isValidFileName(const std::string& name, std::string& message);

    std::uint32_t nextFileId_ = 1;
    std::vector<VirtualFile> files_;
};

} // namespace oscore
