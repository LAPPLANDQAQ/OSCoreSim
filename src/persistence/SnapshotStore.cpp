#include "persistence/SnapshotStore.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace oscore {

namespace {

constexpr std::array<char, 8> kMagic{'O', 'S', 'S', 'M', '2', '0', '2', '6'};
constexpr std::uint32_t kVersionV1 = 1;
constexpr std::uint32_t kVersionV2 = 2;
constexpr std::uint32_t kHeaderSize = 20;
constexpr std::uint32_t kFlags = 0;
constexpr std::uint32_t kMaxStringLength = 1024 * 1024;
constexpr std::uint32_t kMaxVectorCount = 1'000'000;

// 快照文件采用“固定头 + 显式字段序列化”，避免直接 dump STL/对象内存导致 ABI、指针和 padding 不稳定。
class BinaryWriter {
public:
    explicit BinaryWriter(std::ostream& stream) : stream_(stream) {}

    void writeBytes(const char* data, std::size_t size) {
        stream_.write(data, static_cast<std::streamsize>(size));
        if (!stream_) {
            throw std::runtime_error("failed write");
        }
    }

    void writeUint32(std::uint32_t value) {
        writeBytes(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void writeUint64(std::uint64_t value) {
        writeBytes(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void writeInt32(std::int32_t value) {
        writeBytes(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void writeBool(bool value) {
        const std::uint32_t encoded = value ? 1U : 0U;
        writeUint32(encoded);
    }

    void writeString(const std::string& value) {
        // std::string 不能直接 dump 对象内存，统一采用 uint32 length + raw bytes。
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("string too large");
        }
        writeUint32(static_cast<std::uint32_t>(value.size()));
        if (!value.empty()) {
            writeBytes(value.data(), value.size());
        }
    }

private:
    std::ostream& stream_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::istream& stream) : stream_(stream) {}

    void readBytes(char* data, std::size_t size) {
        stream_.read(data, static_cast<std::streamsize>(size));
        if (stream_.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error("truncated file");
        }
    }

    [[nodiscard]] std::uint32_t readUint32() {
        std::uint32_t value = 0;
        readBytes(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    [[nodiscard]] std::uint64_t readUint64() {
        std::uint64_t value = 0;
        readBytes(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    [[nodiscard]] std::int32_t readInt32() {
        std::int32_t value = 0;
        readBytes(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    [[nodiscard]] bool readBool() {
        const auto encoded = readUint32();
        if (encoded > 1) {
            throw std::runtime_error("invalid bool value");
        }
        return encoded == 1;
    }

    [[nodiscard]] std::string readString() {
        // 读取长度前先做上限检查，避免损坏文件导致超大内存分配。
        const auto length = readUint32();
        if (length > kMaxStringLength) {
            throw std::runtime_error("string length is too large");
        }

        std::string value(length, '\0');
        if (length > 0) {
            readBytes(value.data(), length);
        }
        return value;
    }

private:
    std::istream& stream_;
};

void writeHeader(BinaryWriter& writer) {
    // magic 用来快速识别本项目快照文件；version 用来决定后续字段是否包含扩展段。
    writer.writeBytes(kMagic.data(), kMagic.size());
    writer.writeUint32(kSnapshotVersion);  // 当前版本 = 2
    writer.writeUint32(kHeaderSize);
    writer.writeUint32(kFlags);
}

// 读取文件头，返回解析到的版本号（1 或 2），失败时抛异常
[[nodiscard]] std::uint32_t readHeader(BinaryReader& reader) {
    std::array<char, 8> magic{};
    reader.readBytes(magic.data(), magic.size());
    if (magic != kMagic) {
        throw std::runtime_error("invalid magic");
    }

    const auto version = reader.readUint32();
    // 兼容版本 1 和版本 2
    if (version != kVersionV1 && version != kVersionV2) {
        throw std::runtime_error("unsupported version");
    }

    const auto headerSize = reader.readUint32();
    if (headerSize != kHeaderSize) {
        throw std::runtime_error("invalid header size");
    }

    (void)reader.readUint32();  // flags，当前未使用
    return version;
}

// === VFS 二进制序列化 ===

void writeVirtualFile(BinaryWriter& writer, const VirtualFile& file) {
    // VFS 是版本 2 新增扩展，仍沿用长度前缀字符串，保证文件名和内容可以包含空格。
    writer.writeUint32(file.fileId);
    writer.writeString(file.owner);
    writer.writeString(file.name);
    writer.writeString(file.content);
    writer.writeUint64(static_cast<std::uint64_t>(file.createdAt));
    writer.writeUint64(static_cast<std::uint64_t>(file.modifiedAt));
}

VirtualFile readVirtualFile(BinaryReader& reader) {
    VirtualFile file;
    file.fileId = reader.readUint32();
    file.owner = reader.readString();
    file.name = reader.readString();
    file.content = reader.readString();
    file.createdAt = reader.readUint64();
    file.modifiedAt = reader.readUint64();
    return file;
}

void writeUser(BinaryWriter& writer, const UserAccount& account) {
    // 用户、PCB、内存块逐字段写入，格式清晰且便于做版本兼容检查。
    writer.writeString(account.username);
    writer.writeString(account.passwordHash);
    writer.writeString(account.salt);
    writer.writeInt32(static_cast<std::int32_t>(account.failedAttempts));
    writer.writeInt32(static_cast<std::int32_t>(account.status));
}

UserAccount readUser(BinaryReader& reader) {
    UserAccount account;
    account.username = reader.readString();
    account.passwordHash = reader.readString();
    account.salt = reader.readString();
    account.failedAttempts = reader.readInt32();
    const auto status = reader.readInt32();
    if (status < static_cast<std::int32_t>(AccountStatus::NORMAL) ||
        status > static_cast<std::int32_t>(AccountStatus::LOCKED)) {
        throw std::runtime_error("invalid account status");
    }
    account.status = static_cast<AccountStatus>(status);
    return account;
}

// 序列化 PCB 到二进制流。
// 字段顺序（必须与 readPcb 完全一致）：
//   pid(4B) → ppid(4B) → name(str) → owner(str) → state(4B) → priority(4B) → queueLevel(4B)
//   → totalTime(4B) → executedTime(4B) → remainingTime(4B) → timeSliceLeft(4B)
//   → memStart(4B) → memSize(4B) → swappedOut(bool) → childCount(4B) + 逐个 child(4B)
// 枚举值转换为 int32_t 存储，加载时需进行范围校验。
void writePcb(BinaryWriter& writer, const PCB& pcb) {
    writer.writeUint32(pcb.pid);
    writer.writeUint32(pcb.ppid);
    writer.writeString(pcb.name);
    writer.writeString(pcb.owner);
    writer.writeInt32(static_cast<std::int32_t>(pcb.state));
    writer.writeInt32(static_cast<std::int32_t>(pcb.priority));
    writer.writeInt32(static_cast<std::int32_t>(pcb.queueLevel));
    writer.writeUint32(pcb.totalTime);
    writer.writeUint32(pcb.executedTime);
    writer.writeUint32(pcb.remainingTime);
    writer.writeUint32(pcb.timeSliceLeft);
    writer.writeUint32(pcb.memStart);
    writer.writeUint32(pcb.memSize);
    writer.writeBool(pcb.swappedOut);
    writer.writeUint32(static_cast<std::uint32_t>(pcb.children.size()));
    for (const auto child : pcb.children) {
        writer.writeUint32(child);
    }
}

// 从二进制流反序列化 PCB。字段顺序必须与 writePcb 完全一致。
// 关键校验：
//   - ProcessState 值必须在 [NEW, SWAPPED] 范围内，防止损坏文件注入非法状态
//   - children 数量受 kMaxVectorCount 限制，防止恶意文件分配超大内存
PCB readPcb(BinaryReader& reader) {
    PCB pcb;
    pcb.pid = reader.readUint32();
    pcb.ppid = reader.readUint32();
    pcb.name = reader.readString();
    pcb.owner = reader.readString();
    const auto state = reader.readInt32();
    if (state < static_cast<std::int32_t>(ProcessState::NEW) ||
        state > static_cast<std::int32_t>(ProcessState::SWAPPED)) {
        throw std::runtime_error("invalid process state");
    }
    pcb.state = static_cast<ProcessState>(state);
    pcb.priority = reader.readInt32();
    pcb.queueLevel = reader.readInt32();
    pcb.totalTime = reader.readUint32();
    pcb.executedTime = reader.readUint32();
    pcb.remainingTime = reader.readUint32();
    pcb.timeSliceLeft = reader.readUint32();
    pcb.memStart = reader.readUint32();
    pcb.memSize = reader.readUint32();
    pcb.swappedOut = reader.readBool();

    const auto childCount = reader.readUint32();
    if (childCount > kMaxVectorCount) {
        throw std::runtime_error("too many child PIDs");
    }
    pcb.children.reserve(childCount);
    for (std::uint32_t i = 0; i < childCount; ++i) {
        pcb.children.push_back(reader.readUint32());
    }
    return pcb;
}

void writeMemoryBlock(BinaryWriter& writer, const MemoryBlock& block) {
    writer.writeUint32(block.start);
    writer.writeUint32(block.size);
    writer.writeInt32(static_cast<std::int32_t>(block.type));
    writer.writeUint32(block.pid);
    writer.writeString(block.owner);
    writer.writeString(block.tag);
}

MemoryBlock readMemoryBlock(BinaryReader& reader) {
    MemoryBlock block;
    block.start = reader.readUint32();
    block.size = reader.readUint32();
    const auto type = reader.readInt32();
    if (type < static_cast<std::int32_t>(MemoryBlockType::FREE) ||
        type > static_cast<std::int32_t>(MemoryBlockType::SWAPPED)) {
        throw std::runtime_error("invalid memory block type");
    }
    block.type = static_cast<MemoryBlockType>(type);
    block.pid = reader.readUint32();
    block.owner = reader.readString();
    block.tag = reader.readString();
    return block;
}

std::filesystem::path tempPathFor(const std::string& path) {
    auto result = std::filesystem::path(path);
    result.replace_extension(".tmp");
    return result;
}

} // namespace

SnapshotStore::SnapshotStore(std::string path) : path_(std::move(path)) {}

std::string SnapshotStore::defaultPath() const {
    return path_;
}

bool SnapshotStore::exists() const {
    std::error_code error;
    return std::filesystem::exists(path_, error);
}

SnapshotSummary SnapshotStore::summarize(const KernelSnapshot& snapshot) const {
    SnapshotSummary summary;
    summary.users = snapshot.users.size();
    summary.processes = snapshot.pcbs.size();
    summary.memoryBlocks = snapshot.memoryBlocks.size();
    summary.vfsFiles = snapshot.virtualFiles.size();
    for (std::size_t i = 0; i < snapshot.readyQueues.size(); ++i) {
        summary.readyQueueSizes[i] = snapshot.readyQueues[i].size();
    }
    return summary;
}

// save()：保存系统状态到二进制快照文件。
//
// 原子写入策略（防止写入中断导致快照损坏）：
//   1. 创建临时文件 path_.tmp
//   2. 将所有数据写入临时文件
//   3. flush 临时文件确保数据落盘
//   4. 删除旧的正式文件（如有），将 .tmp rename 为正式文件
//
// 二进制格式（v2，按固定顺序写入）：
//   文件头（20B）：Magic(8B "OSSM2026") + Version(4B=2) + HeaderSize(4B=20) + Flags(4B=0)
//   用户段：userCount(4B) + 逐个 UserAccount
//   进程段：nextPid(4B) + pcbCount(4B) + 逐个 PCB（含 children 列表）
//   就绪队列段：Q0.size + Q0 PIDs + Q1.size + Q1 PIDs + Q2.size + Q2 PIDs
//   内存段：totalMemoryKB(4B) + algorithm(4B) + blockCount(4B) + 逐个 MemoryBlock
//   调度段：schedulerRunning(bool) + schedulerOwner(string)
//   VFS段（v2 新增）：nextFileId(4B) + fileCount(4B) + 逐个 VirtualFile
//
// 所有整数使用原生 little-endian，字符串使用 uint32_t length + raw bytes。
bool SnapshotStore::save(const KernelSnapshot& snapshot, std::string& message) const {
    try {
        const std::filesystem::path finalPath(path_);
        const auto tmpPath = tempPathFor(path_);
        if (finalPath.has_parent_path()) {
            std::filesystem::create_directories(finalPath.parent_path());
        }

        {
            std::ofstream output(tmpPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                message = "[失败] 保存失败：无法打开临时快照文件。";
                return false;
            }

            BinaryWriter writer(output);
            // 二进制格式面向本课程 Windows 环境，使用带版本号的原生小端存储；跨端序转换不在本阶段范围内。
            writeHeader(writer);

            // 写入顺序就是磁盘格式的一部分：load 必须按完全相同的顺序读取。
            writer.writeUint32(static_cast<std::uint32_t>(snapshot.users.size()));
            for (const auto& account : snapshot.users) {
                writeUser(writer, account);
            }

            writer.writeUint32(snapshot.nextPid);
            writer.writeUint32(static_cast<std::uint32_t>(snapshot.pcbs.size()));
            for (const auto& pcb : snapshot.pcbs) {
                writePcb(writer, pcb);
            }

            for (const auto& queue : snapshot.readyQueues) {
                writer.writeUint32(static_cast<std::uint32_t>(queue.size()));
                for (const auto pid : queue) {
                    writer.writeUint32(pid);
                }
            }

            writer.writeUint32(snapshot.totalMemoryKB);
            writer.writeInt32(static_cast<std::int32_t>(snapshot.allocAlgorithm));
            writer.writeUint32(static_cast<std::uint32_t>(snapshot.memoryBlocks.size()));
            for (const auto& block : snapshot.memoryBlocks) {
                writeMemoryBlock(writer, block);
            }

            writer.writeBool(snapshot.schedulerRunning);
            writer.writeString(snapshot.schedulerOwner);

            // Version 2 VFS 扩展：追加 nextFileId 和所有虚拟文件；不改变前面 V1 字段布局。
            writer.writeUint32(snapshot.nextFileId);
            writer.writeUint32(static_cast<std::uint32_t>(snapshot.virtualFiles.size()));
            for (const auto& file : snapshot.virtualFiles) {
                writeVirtualFile(writer, file);
            }

            output.flush();
            if (!output) {
                message = "[失败] 保存失败：无法刷新快照文件。";
                return false;
            }
        }

        std::error_code error;
        std::filesystem::remove(finalPath, error);
        error.clear();
        std::filesystem::rename(tmpPath, finalPath, error);
        if (error) {
            message = "[失败] 保存失败：无法替换快照文件: " + error.message();
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        message = std::string("[失败] 保存失败: ") + ex.what();
        return false;
    }
}

// load()：从二进制快照文件加载系统状态。
//
// 读取流程（必须与 save 完全相同的顺序）：
//   1. 打开文件（只读二进制模式）
//   2. 读取文件头，验证 Magic="OSSM2026"、版本号（支持 v1 和 v2）
//   3. 按 save 的写入顺序反序列化各数据段
//   4. 版本兼容：v1 快照没有 VFS 段，VFS 字段保持默认空值
//
// 版本号决定后续字段的读取范围：v1 版本读取到调度段为止，v2 版本继续读取 VFS 扩展段。
bool SnapshotStore::load(KernelSnapshot& snapshot, std::string& message) const {
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            message = "[失败] 加载失败：快照文件不存在或无法打开。";
            return false;
        }

        KernelSnapshot loaded;
        BinaryReader reader(input);
        const auto fileVersion = readHeader(reader);  // 返回 1 或 2

        const auto userCount = reader.readUint32();
        if (userCount > kMaxVectorCount) {
            throw std::runtime_error("too many users");
        }
        loaded.users.reserve(userCount);
        for (std::uint32_t i = 0; i < userCount; ++i) {
            loaded.users.push_back(readUser(reader));
        }

        loaded.nextPid = reader.readUint32();
        const auto pcbCount = reader.readUint32();
        if (pcbCount > kMaxVectorCount) {
            throw std::runtime_error("too many PCBs");
        }
        loaded.pcbs.reserve(pcbCount);
        for (std::uint32_t i = 0; i < pcbCount; ++i) {
            loaded.pcbs.push_back(readPcb(reader));
        }

        for (auto& queue : loaded.readyQueues) {
            const auto queueCount = reader.readUint32();
            if (queueCount > kMaxVectorCount) {
                throw std::runtime_error("too many ready queue entries");
            }
            queue.reserve(queueCount);
            for (std::uint32_t i = 0; i < queueCount; ++i) {
                queue.push_back(reader.readUint32());
            }
        }

        loaded.totalMemoryKB = reader.readUint32();
        const auto algorithm = reader.readInt32();
        if (algorithm < static_cast<std::int32_t>(AllocAlgorithm::FIRST_FIT) ||
            algorithm > static_cast<std::int32_t>(AllocAlgorithm::WORST_FIT)) {
            throw std::runtime_error("invalid allocation algorithm");
        }
        loaded.allocAlgorithm = static_cast<AllocAlgorithm>(algorithm);

        const auto blockCount = reader.readUint32();
        if (blockCount > kMaxVectorCount) {
            throw std::runtime_error("too many memory blocks");
        }
        loaded.memoryBlocks.reserve(blockCount);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            loaded.memoryBlocks.push_back(readMemoryBlock(reader));
        }

        loaded.schedulerRunning = reader.readBool();
        loaded.schedulerOwner = reader.readString();

        // Version 1 快照没有 VFS 段，保持默认空文件系统；Version 2 才继续读取扩展字段。
        if (fileVersion >= 2) {
            loaded.nextFileId = reader.readUint32();
            const auto vfsCount = reader.readUint32();
            if (vfsCount > kMaxVectorCount) {
                throw std::runtime_error("too many virtual files");
            }
            loaded.virtualFiles.reserve(vfsCount);
            for (std::uint32_t i = 0; i < vfsCount; ++i) {
                loaded.virtualFiles.push_back(readVirtualFile(reader));
            }
        }

        snapshot = std::move(loaded);
        return true;
    } catch (const std::exception& ex) {
        message = std::string("[失败] 加载失败: ") + ex.what();
        return false;
    }
}

} // namespace oscore
