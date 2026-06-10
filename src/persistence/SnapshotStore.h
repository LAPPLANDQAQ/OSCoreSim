#pragma once

#include "auth/UserAccount.h"
#include "memory/MemoryBlock.h"
#include "process/PCB.h"
#include "vfs/VirtualFileSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oscore {

// 当前快照文件版本号：版本 2（末尾追加了 VFS 数据段）
// 版本 1 兼容读取：VFS 字段保持默认空值，不报错
constexpr std::uint32_t kSnapshotVersion = 2;

// KernelSnapshot：完整系统状态快照。
// 包含用户、PCB、就绪队列、内存块、调度元数据、VFS 文件的全部数据。
// 使用简单值类型和 vector，便于显式长度前缀序列化，不直接 dump 对象内存。
struct KernelSnapshot {
    std::vector<UserAccount> users;                          // 全部注册用户（含锁定状态）
    std::uint32_t nextPid = 1;                               // 下一个可分配的 PID
    std::vector<PCB> pcbs;                                   // 全部 PCB 记录
    std::array<std::vector<std::uint32_t>, 3> readyQueues;   // Q0/Q1/Q2 就绪队列 PID 列表
    std::vector<MemoryBlock> memoryBlocks;                   // 内存块表
    std::uint32_t totalMemoryKB = 1024;                      // 总内存量
    AllocAlgorithm allocAlgorithm = AllocAlgorithm::FIRST_FIT;  // 当前分配算法
    bool schedulerRunning = false;                           // 调度器是否运行中
    std::string schedulerOwner;                              // 调度器所有者
    std::uint32_t nextFileId = 1;                            // 下一个虚拟文件 ID
    std::vector<VirtualFile> virtualFiles;                   // 全部虚拟文件
};

// SnapshotSummary：快照摘要，用于 save/load 后的状态报告。
struct SnapshotSummary {
    std::size_t users = 0;                    // 快照中的用户数量。
    std::size_t processes = 0;                // 快照中的 PCB 数量。
    std::size_t memoryBlocks = 0;             // 快照中的内存块数量。
    std::size_t vfsFiles = 0;                 // 快照中的虚拟文件数量。
    std::array<std::size_t, 3> readyQueueSizes{};  // Q0/Q1/Q2 各自保存的 PID 数量。
};

// SnapshotStore：二进制快照文件读写器。
//
// 文件格式（v2）：
//   - 文件头（20 字节）：Magic(8B "OSSM2026") + Version(4B=2) + HeaderSize(4B=20) + Flags(4B=0)
//   - 数据段按固定顺序写入：用户列表 → nextPid → PCB 列表 → 就绪队列 → 内存块 → 调度状态 → VFS
//   - 所有字段使用显式长度前缀：整数为原生 little-endian，字符串为 uint32 length + bytes
//   - 原子写入：先写 .tmp 文件，成功后 rename 替换正式文件
class SnapshotStore {
public:
    explicit SnapshotStore(std::string path = "data/os_state.bin");

    [[nodiscard]] std::string defaultPath() const;
    [[nodiscard]] bool exists() const;
    [[nodiscard]] SnapshotSummary summarize(const KernelSnapshot& snapshot) const;

    // 保存快照：先写临时文件，成功后原子替换，防止写入中断导致快照损坏
    bool save(const KernelSnapshot& snapshot, std::string& message) const;
    // 加载快照：读取文件头验证版本，按固定顺序反序列化各数据段
    bool load(KernelSnapshot& snapshot, std::string& message) const;

private:
    std::string path_;  // 快照文件路径，默认 "data/os_state.bin"
};

} // namespace oscore
