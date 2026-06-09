#include "memory/MemoryManager.h"

#include "util/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace oscore {

MemoryManager::MemoryManager() {
    blocks_.push_back(MemoryBlock{0, totalMemoryKB_, MemoryBlockType::FREE, 0, "", ""});
}

bool MemoryManager::allocateManual(
    const std::string& owner,
    std::uint32_t sizeKB,
    std::uint32_t& outStart,
    std::string& message) {
    return allocateManual(owner, "manual", sizeKB, outStart, message);
}

bool MemoryManager::allocateManual(
    const std::string& owner,
    const std::string& tag,
    std::uint32_t sizeKB,
    std::uint32_t& outStart,
    std::string& message) {
    return allocateLocked(owner, 0, tag, sizeKB, MemoryBlockType::KERNEL, outStart, message);
}

bool MemoryManager::allocateForProcess(
    const std::string& owner,
    std::uint32_t pid,
    const std::string& processName,
    std::uint32_t sizeKB,
    std::uint32_t& outStart,
    std::string& message) {
    return allocateLocked(owner, pid, processName, sizeKB, MemoryBlockType::PROCESS, outStart, message);
}

bool MemoryManager::freeByAddress(const std::string& owner, std::uint32_t addr, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(blocks_.begin(), blocks_.end(), [addr](const MemoryBlock& block) {
        return block.start == addr;
    });

    if (it == blocks_.end()) {
        message = "[失败] 释放失败：没有内存块起始于该地址。";
        return false;
    }
    if (it->type == MemoryBlockType::FREE) {
        message = "[失败] 释放失败：目标内存块已是空闲状态。";
        return false;
    }
    if (it->owner != owner) {
        message = "[失败] 释放失败：该内存块属于其他用户。";
        return false;
    }
    if (it->type == MemoryBlockType::PROCESS) {
        message = "[失败] 释放失败：进程内存必须通过 kill_pcb 或 swap_out 释放。";
        return false;
    }

    const auto released = it->size;
    *it = MemoryBlock{it->start, it->size, MemoryBlockType::FREE, 0, "", ""};
    mergeFreeBlocksLocked();

    std::ostringstream output;
    output << "[成功] 释放内存：起始地址=" << addr << "KB, 大小=" << released << "KB。";
    message = output.str();
    return true;
}

bool MemoryManager::freeByPid(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(blocks_.begin(), blocks_.end(), [owner, pid](const MemoryBlock& block) {
        return block.type == MemoryBlockType::PROCESS && block.owner == owner && block.pid == pid;
    });
    if (it == blocks_.end()) {
        message = "[失败] 释放失败：未找到进程内存块。";
        return false;
    }

    const auto released = it->size;
    const auto start = it->start;
    *it = MemoryBlock{it->start, it->size, MemoryBlockType::FREE, 0, "", ""};
    mergeFreeBlocksLocked();

    std::ostringstream output;
    output << "[成功] 释放 PID=" << pid << " 内存：起始地址=" << start << "KB, 大小=" << released << "KB。";
    message = output.str();
    return true;
}

bool MemoryManager::swapOutProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(blocks_.begin(), blocks_.end(), [owner, pid](const MemoryBlock& block) {
        return block.type == MemoryBlockType::PROCESS && block.owner == owner && block.pid == pid;
    });
    if (it == blocks_.end()) {
        message = "[失败] 换出失败：未找到进程内存块或已换出。";
        return false;
    }

    const auto released = it->size;
    *it = MemoryBlock{it->start, it->size, MemoryBlockType::FREE, 0, "", ""};
    mergeFreeBlocksLocked();

    std::ostringstream output;
    output << "[成功] PID=" << pid << " 已换出。释放物理内存 " << released << "KB。";
    message = output.str();
    return true;
}

// compact（内存紧缩）：将所有已分配块向低地址端移动，消除空闲块间隙。
// 算法：
//   1. 收集所有非 FREE 块到 allocated 向量中（保持原相对顺序）
//   2. cursor 从 0 开始，逐个重排 allocated 块的 start 到 cursor
//   3. 记录 PROCESS 块的新地址到 pidNewStart（Kernel 据此回写 PCB::memStart）
//   4. 如果 cursor 后还有剩余空间，追加一个 FREE 块覆盖剩余部分
// 例如：[P50][FREE30][P40] → [P50][P40][FREE20]
CompactionResult MemoryManager::compact() {
    std::lock_guard<std::mutex> lock(mutex_);
    CompactionResult result;
    result.success = true;

    std::vector<MemoryBlock> allocated;
    allocated.reserve(blocks_.size());
    for (const auto& block : blocks_) {
        if (block.type != MemoryBlockType::FREE) {
            allocated.push_back(block);
        }
    }

    std::uint32_t cursor = 0;
    std::ostringstream output;
    output << "[成功] 内存紧缩完成。";
    for (auto& block : allocated) {
        const auto oldStart = block.start;
        block.start = cursor;
        cursor += block.size;
        if (oldStart != block.start) {
            output << "\n移动 " << toString(block.type)
                   << " 标签=" << block.tag
                   << " pid=" << block.pid
                   << ": " << oldStart << "KB -> " << block.start << "KB";
        }
        if (block.type == MemoryBlockType::PROCESS) {
            result.pidNewStart[block.pid] = block.start;
        }
    }

    blocks_ = std::move(allocated);
    if (cursor < totalMemoryKB_) {
        blocks_.push_back(MemoryBlock{cursor, totalMemoryKB_ - cursor, MemoryBlockType::FREE, 0, "", ""});
    }
    if (blocks_.empty()) {
        blocks_.push_back(MemoryBlock{0, totalMemoryKB_, MemoryBlockType::FREE, 0, "", ""});
    }

    result.message = output.str();
    return result;
}

std::string MemoryManager::showMemory(const std::string& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;

    std::uint32_t usedKB = 0;
    std::uint32_t freeKB = 0;
    for (const auto& block : blocks_) {
        if (block.type == MemoryBlockType::FREE) {
            freeKB += block.size;
        } else {
            usedKB += block.size;
        }
    }

    output << "Memory Layout [Total: " << totalMemoryKB_
           << " KB | Used: " << usedKB
           << " KB | Free: " << freeKB
           << " KB | Algo: " << toString(algorithm_)
           << "]\n\n";

    output << std::left
           << padRightDisplayWidth("Address", 18)
           << padRightDisplayWidth("Size", 10)
           << padRightDisplayWidth("Type", 10)
           << padRightDisplayWidth("Owner", 12)
           << padRightDisplayWidth("PID", 6)
           << "Tag\n"
           << std::string(62, '-') << '\n';

    for (const auto& block : blocks_) {
        const bool free = block.type == MemoryBlockType::FREE;
        const bool owned = !free && block.owner == owner;
        const auto visibleOwner = free ? "-" : (owned ? block.owner : "OTHER_USER");
        const auto visiblePid = free ? "-" : std::to_string(block.pid);
        const auto visibleTag = free ? "-" : block.tag;

        std::ostringstream addrRange;
        addrRange << std::setfill('0') << std::setw(4) << block.start
                  << " - "
                  << std::setfill('0') << std::setw(4) << (block.start + block.size - 1)
                  << " KB";
        std::ostringstream sizeStr;
        sizeStr << block.size << " KB";

        output << std::left
               << padRightDisplayWidth(addrRange.str(), 18)
               << padRightDisplayWidth(sizeStr.str(), 10)
               << padRightDisplayWidth(free ? "Free" : toString(block.type), 10)
               << padRightDisplayWidth(visibleOwner, 12)
               << padRightDisplayWidth(visiblePid, 6)
               << visibleTag << '\n';
    }

    const int mapWidth = 64;
    output << "\nMemory Map:\n";
    std::string mapLine;
    mapLine.reserve(static_cast<std::size_t>(mapWidth) + 2);
    for (const auto& block : blocks_) {
        const int blockChars = std::max(1,
            static_cast<int>(static_cast<double>(block.size) / totalMemoryKB_ * mapWidth));
        if (block.type == MemoryBlockType::FREE) {
            mapLine.append(static_cast<std::size_t>(blockChars), '.');
        } else if (block.type == MemoryBlockType::PROCESS) {
            mapLine.append(static_cast<std::size_t>(blockChars), 'P');
        } else {
            mapLine.append(static_cast<std::size_t>(blockChars), 'K');
        }
    }
    if (mapLine.size() > static_cast<std::size_t>(mapWidth)) {
        mapLine.resize(static_cast<std::size_t>(mapWidth));
    }
    while (mapLine.size() < static_cast<std::size_t>(mapWidth)) {
        mapLine.push_back('.');
    }
    output << '[' << mapLine << "]\n"
           << "Legend: P=Process, K=Manual/Kernel, .=Free";
    return output.str();
}

std::string MemoryManager::memoryStat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t used = 0;
    std::uint32_t free = 0;
    std::uint32_t largestFree = 0;
    std::size_t freeBlocks = 0;
    std::size_t allocatedBlocks = 0;

    for (const auto& block : blocks_) {
        if (block.type == MemoryBlockType::FREE) {
            free += block.size;
            largestFree = std::max(largestFree, block.size);
            ++freeBlocks;
        } else {
            used += block.size;
            ++allocatedBlocks;
        }
    }

    const double fragmentation = free == 0 ? 0.0 : (1.0 - static_cast<double>(largestFree) / free) * 100.0;
    std::ostringstream output;
    output << "=== 内存统计 / Memory Statistics ===\n"
           << "内存总量: " << totalMemoryKB_ << " KB\n"
           << "已用内存: " << used << " KB\n"
           << "空闲内存: " << free << " KB\n"
           << "已分配块: " << allocatedBlocks << '\n'
           << "空闲块: " << freeBlocks << '\n'
           << "最大空闲块: " << largestFree << " KB\n"
           << "外部碎片率: " << std::fixed << std::setprecision(2) << fragmentation << "%";
    return output.str();
}

bool MemoryManager::setAlgorithm(const std::string& algoName, std::string& message) {
    const auto normalized = normalizeAlgorithmName(algoName);
    std::lock_guard<std::mutex> lock(mutex_);
    if (normalized == "FF" || normalized == "FIRST" || normalized == "FIRST_FIT") {
        algorithm_ = AllocAlgorithm::FIRST_FIT;
    } else if (normalized == "BF" || normalized == "BEST" || normalized == "BEST_FIT") {
        algorithm_ = AllocAlgorithm::BEST_FIT;
    } else if (normalized == "WF" || normalized == "WORST" || normalized == "WORST_FIT") {
        algorithm_ = AllocAlgorithm::WORST_FIT;
    } else {
        message = "[失败] 切换算法失败：支持的值为 FF, BF, WF。";
        return false;
    }

    message = std::string("[成功] 分配算法已切换为 ") + toString(algorithm_) + "。";
    return true;
}

AllocAlgorithm MemoryManager::currentAlgorithm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return algorithm_;
}

std::string MemoryManager::currentAlgorithmName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return toString(algorithm_);
}

std::uint32_t MemoryManager::totalMemoryKB() const {
    return totalMemoryKB_;
}

void MemoryManager::setTotalMemoryKB(std::uint32_t totalMemoryKB) {
    std::lock_guard<std::mutex> lock(mutex_);
    totalMemoryKB_ = totalMemoryKB;
}

std::uint32_t MemoryManager::usedMemoryKB() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t used = 0;
    for (const auto& block : blocks_) {
        if (block.type != MemoryBlockType::FREE) {
            used += block.size;
        }
    }
    return used;
}

std::uint32_t MemoryManager::freeMemoryKB() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t free = 0;
    for (const auto& block : blocks_) {
        if (block.type == MemoryBlockType::FREE) {
            free += block.size;
        }
    }
    return free;
}

void MemoryManager::setAlgorithmDirect(AllocAlgorithm algorithm) {
    std::lock_guard<std::mutex> lock(mutex_);
    algorithm_ = algorithm;
}

bool MemoryManager::validateBlocks(std::string& message) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (totalMemoryKB_ == 0) {
        message = "[错误] 内存校验失败：总内存必须大于 0。";
        return false;
    }
    if (blocks_.empty()) {
        message = "[错误] 内存校验失败：内存块表为空。";
        return false;
    }

    std::uint32_t expectedStart = 0;
    for (const auto& block : blocks_) {
        if (block.size == 0) {
            message = "[错误] 内存校验失败：内存块大小不能为 0。";
            return false;
        }
        if (block.start != expectedStart) {
            message = "[错误] 内存校验失败：内存块不连续或未排序。";
            return false;
        }
        if (block.size > totalMemoryKB_ || block.start > totalMemoryKB_ - block.size) {
            message = "[错误] 内存校验失败：内存块超出总内存范围。";
            return false;
        }
        if (block.type == MemoryBlockType::FREE && (block.pid != 0 || !block.owner.empty())) {
            message = "[错误] 内存校验失败：空闲块包含所有者或 PID。";
            return false;
        }
        expectedStart = block.start + block.size;
    }

    if (expectedStart != totalMemoryKB_) {
        message = "[错误] 内存校验失败：内存块未覆盖总内存。";
        return false;
    }

    message = "[提示] 内存校验通过。";
    return true;
}

std::vector<MemoryBlock> MemoryManager::exportBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_;
}

void MemoryManager::importBlocks(const std::vector<MemoryBlock>& blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    blocks_ = blocks;
    sortBlocksLocked();
    mergeFreeBlocksLocked();
}

// ============================================================================
// 动态分区分配核心算法
// ============================================================================
// allocateLocked 是三种分配接口（手动、进程、换出恢复）的共同底层实现。
// 算法流程：
//   1. 基础校验：owner 非空（已登录）、sizeKB > 0
//   2. 调用 findFreeBlockLocked 根据当前算法（FF/BF/WF）查找合适的空闲块
//   3. 如果空闲块大小正好等于请求大小 → 直接标记为已分配
//   4. 如果空闲块更大 → 拆分为"已分配块 + 剩余空闲块"，插入到 blocks_ 中
//   5. 最后排序 blocks_ 以保持起始地址升序
// 注意：分配成功后不会自动合并 FREE 块（仅释放时合并），紧凑（compact）可消除碎片。
bool MemoryManager::allocateLocked(
    const std::string& owner,
    std::uint32_t pid,
    const std::string& tag,
    std::uint32_t sizeKB,
    MemoryBlockType type,
    std::uint32_t& outStart,
    std::string& message) {
    if (owner.empty()) {
        message = "[失败] 分配失败：请先登录。";
        return false;
    }
    if (sizeKB == 0) {
        message = "[失败] 分配失败：内存大小必须大于 0 KB。";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = findFreeBlockLocked(sizeKB);
    if (it == blocks_.end()) {
        message = "[失败] 分配失败：没有合适的空闲内存块。";
        return false;
    }

    const auto freeStart = it->start;
    const auto freeSize = it->size;
    MemoryBlock allocated{freeStart, sizeKB, type, pid, owner, tag};
    if (freeSize == sizeKB) {
        *it = allocated;
    } else {
        it->start = freeStart + sizeKB;
        it->size = freeSize - sizeKB;
        blocks_.insert(it, allocated);
    }
    sortBlocksLocked();

    outStart = freeStart;
    std::ostringstream output;
    output << "[成功] 分配 " << sizeKB << "KB 内存，起始地址=" << outStart << "KB，算法=" << toString(algorithm_) << "。";
    message = output.str();
    return true;
}

// ============================================================================
// FF/BF/WF 空闲块选择算法
// ============================================================================
// 根据当前算法（algorithm_）从 blocks_ 中选择合适的空闲块：
//
// FF（首次适应 First Fit）：
//   遍历 blocks_，返回第一个 type==FREE 且 size >= sizeKB 的块。
//   时间复杂度 O(n)，实际平均最快（空闲块通常在前面或中间）。
//   缺点：可能将大块切碎，产生外部碎片。
//
// BF（最佳适应 Best Fit）：
//   遍历全部 blocks_，选择 size >= sizeKB 的最小空闲块。
//   时间复杂度 O(n)，每次都遍历全部。
//   优点：减少内部浪费；缺点：留下越来越小的碎片，外部碎片可能增加。
//
// WF（最差适应 Worst Fit）：
//   遍历全部 blocks_，选择 size >= sizeKB 的最大空闲块。
//   时间复杂度 O(n)，每次都遍历全部。
//   优点：切大块后可能保留较大剩余空间；缺点：大块被快速消耗。
std::vector<MemoryBlock>::iterator MemoryManager::findFreeBlockLocked(std::uint32_t sizeKB) {
    std::vector<MemoryBlock>::iterator best = blocks_.end();
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->type != MemoryBlockType::FREE || it->size < sizeKB) {
            continue;
        }

        if (algorithm_ == AllocAlgorithm::FIRST_FIT) {
            return it;
        }
        if (best == blocks_.end()) {
            best = it;
            continue;
        }
        if (algorithm_ == AllocAlgorithm::BEST_FIT && it->size < best->size) {
            best = it;
        } else if (algorithm_ == AllocAlgorithm::WORST_FIT && it->size > best->size) {
            best = it;
        }
    }
    return best;
}

void MemoryManager::sortBlocksLocked() {
    std::sort(blocks_.begin(), blocks_.end(), [](const MemoryBlock& left, const MemoryBlock& right) {
        return left.start < right.start;
    });
}

// 合并相邻的空闲块：遍历 blocks_，将地址连续的 FREE 块合并为一个。
// 在每次释放内存后调用，降低外部碎片。
// 注意：只能合并物理地址连续的 FREE 块（start + size == next.start），
//       不会跨越已分配块进行合并。
void MemoryManager::mergeFreeBlocksLocked() {
    sortBlocksLocked();
    std::vector<MemoryBlock> merged;
    for (const auto& block : blocks_) {
        if (!merged.empty() &&
            merged.back().type == MemoryBlockType::FREE &&
            block.type == MemoryBlockType::FREE &&
            merged.back().start + merged.back().size == block.start) {
            merged.back().size += block.size;
        } else {
            merged.push_back(block);
        }
    }
    blocks_ = std::move(merged);
}

std::string MemoryManager::normalizeAlgorithmName(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

} // namespace oscore
