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
    for (std::size_t i = 0; i < snapshot.readyQueues.size(); ++i) {
        summary.readyQueueSizes[i] = snapshot.readyQueues[i].size();
    }
    return summary;
}

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
                message = "Save failed: cannot open temporary snapshot file.";
                return false;
            }

            BinaryWriter writer(output);
            // 二进制格式面向本课程 Windows 环境，使用带版本号的原生小端存储；跨端序转换不在本阶段范围内。
            writeHeader(writer);

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

            // P9 VFS 数据：写入 nextFileId 和所有虚拟文件
            writer.writeUint32(snapshot.nextFileId);
            writer.writeUint32(static_cast<std::uint32_t>(snapshot.virtualFiles.size()));
            for (const auto& file : snapshot.virtualFiles) {
                writeVirtualFile(writer, file);
            }

            output.flush();
            if (!output) {
                message = "Save failed: failed to flush snapshot file.";
                return false;
            }
        }

        std::error_code error;
        std::filesystem::remove(finalPath, error);
        error.clear();
        std::filesystem::rename(tmpPath, finalPath, error);
        if (error) {
            message = "Save failed: cannot replace snapshot file: " + error.message();
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        message = std::string("Save failed: ") + ex.what();
        return false;
    }
}

bool SnapshotStore::load(KernelSnapshot& snapshot, std::string& message) const {
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            message = "Load failed: state file does not exist or cannot be opened.";
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

        // P9 VFS 数据：仅在版本 2 快照中读取；版本 1 保持空 VFS
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
        message = std::string("Load failed: ") + ex.what();
        return false;
    }
}

} // namespace oscore
