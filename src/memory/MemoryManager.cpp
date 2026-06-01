#include "memory/MemoryManager.h"

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
    return allocateLocked(owner, 0, "manual", sizeKB, MemoryBlockType::KERNEL, outStart, message);
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
        message = "Free failed: no memory block starts at the given address.";
        return false;
    }
    if (it->type == MemoryBlockType::FREE) {
        message = "Free failed: target block is already free.";
        return false;
    }
    if (it->owner != owner) {
        message = "Free failed: memory block belongs to another user.";
        return false;
    }
    if (it->type == MemoryBlockType::PROCESS) {
        message = "Free failed: process memory must be released by kill_pcb or swap_out.";
        return false;
    }

    const auto released = it->size;
    *it = MemoryBlock{it->start, it->size, MemoryBlockType::FREE, 0, "", ""};
    mergeFreeBlocksLocked();

    std::ostringstream output;
    output << "[OK] Freed memory at start=" << addr << "KB, size=" << released << "KB.";
    message = output.str();
    return true;
}

bool MemoryManager::freeByPid(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(blocks_.begin(), blocks_.end(), [owner, pid](const MemoryBlock& block) {
        return block.type == MemoryBlockType::PROCESS && block.owner == owner && block.pid == pid;
    });
    if (it == blocks_.end()) {
        message = "Free failed: process memory block not found.";
        return false;
    }

    const auto released = it->size;
    const auto start = it->start;
    *it = MemoryBlock{it->start, it->size, MemoryBlockType::FREE, 0, "", ""};
    mergeFreeBlocksLocked();

    std::ostringstream output;
    output << "[OK] Released PID=" << pid << " memory at start=" << start << "KB, size=" << released << "KB.";
    message = output.str();
    return true;
}

bool MemoryManager::swapOutProcess(const std::string& owner, std::uint32_t pid, std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(blocks_.begin(), blocks_.end(), [owner, pid](const MemoryBlock& block) {
        return block.type == MemoryBlockType::PROCESS && block.owner == owner && block.pid == pid;
    });
    if (it == blocks_.end()) {
        message = "Swap out failed: process memory block not found or already swapped out.";
        return false;
    }

    const auto released = it->size;
    *it = MemoryBlock{it->start, it->size, MemoryBlockType::FREE, 0, "", ""};
    mergeFreeBlocksLocked();

    std::ostringstream output;
    output << "[OK] PID=" << pid << " swapped out. Released " << released << "KB physical memory.";
    message = output.str();
    return true;
}

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
    output << "[OK] Memory compacted.";
    for (auto& block : allocated) {
        const auto oldStart = block.start;
        block.start = cursor;
        cursor += block.size;
        if (oldStart != block.start) {
            output << "\nMoved " << toString(block.type)
                   << " tag=" << block.tag
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
    output << "=== Memory Blocks ===\n"
           << std::left << std::setw(8) << "Start"
           << std::setw(8) << "End"
           << std::setw(9) << "SizeKB"
           << std::setw(10) << "Type"
           << std::setw(12) << "Owner"
           << std::setw(7) << "PID"
           << "Tag\n";

    for (const auto& block : blocks_) {
        const bool free = block.type == MemoryBlockType::FREE;
        const bool owned = !free && block.owner == owner;
        const auto visibleOwner = free ? "-" : (owned ? block.owner : "OTHER_USER");
        const auto visiblePid = free ? "-" : std::to_string(block.pid);
        const auto visibleTag = free ? "-" : block.tag;
        output << std::left << std::setw(8) << block.start
               << std::setw(8) << (block.start + block.size - 1)
               << std::setw(9) << block.size
               << std::setw(10) << toString(block.type)
               << std::setw(12) << visibleOwner
               << std::setw(7) << visiblePid
               << visibleTag << '\n';
    }

    output << "\nMemory Map (0-" << totalMemoryKB_ << "KB):\n";
    for (const auto& block : blocks_) {
        if (block.type == MemoryBlockType::FREE) {
            output << "|--FREE:" << block.size << "KB--";
        } else if (block.type == MemoryBlockType::PROCESS) {
            output << "|##P" << block.pid << ':' << block.size << "KB";
        } else {
            output << "|##" << block.tag << ':' << block.size << "KB";
        }
    }
    output << '|';
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
    output << "=== Memory Statistics ===\n"
           << "Total Memory: " << totalMemoryKB_ << " KB\n"
           << "Used Memory: " << used << " KB\n"
           << "Free Memory: " << free << " KB\n"
           << "Allocated Blocks: " << allocatedBlocks << '\n'
           << "Free Blocks: " << freeBlocks << '\n'
           << "Largest Free Block: " << largestFree << " KB\n"
           << "External Fragmentation: " << std::fixed << std::setprecision(2) << fragmentation << "%";
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
        message = "Set algorithm failed: supported values are FF, BF, WF.";
        return false;
    }

    message = std::string("[OK] Allocation algorithm changed to ") + toString(algorithm_) + ".";
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

bool MemoryManager::allocateLocked(
    const std::string& owner,
    std::uint32_t pid,
    const std::string& tag,
    std::uint32_t sizeKB,
    MemoryBlockType type,
    std::uint32_t& outStart,
    std::string& message) {
    if (owner.empty()) {
        message = "Allocation failed: user must login first.";
        return false;
    }
    if (sizeKB == 0) {
        message = "Allocation failed: sizeKB must be greater than 0.";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = findFreeBlockLocked(sizeKB);
    if (it == blocks_.end()) {
        message = "Allocation failed: no suitable free block.";
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
    output << "[OK] Allocated " << sizeKB << "KB at start=" << outStart << "KB using " << toString(algorithm_) << ".";
    message = output.str();
    return true;
}

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
