#include "view/OverviewRenderer.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace oscore {

std::string OverviewRenderer::render(
    const std::string& currentUser,
    const std::vector<PCB>& userProcesses,
    const std::array<std::vector<std::uint32_t>, 3>& readyQueues,
    const std::vector<MemoryBlock>& memoryBlocks,
    std::uint32_t totalMemoryKB,
    const SchedulerInfo& schedulerInfo,
    const std::string& snapshotPath,
    const std::string& algorithmName) const {
    // 计算内存统计
    std::uint32_t usedKB = 0;
    std::uint32_t freeKB = 0;
    std::uint32_t largestFreeKB = 0;
    for (const auto& block : memoryBlocks) {
        if (block.type == MemoryBlockType::FREE) {
            freeKB += block.size;
            largestFreeKB = std::max(largestFreeKB, block.size);
        } else {
            usedKB += block.size;
        }
    }

    const double fragmentationRate = freeKB == 0 ? 0.0
        : (1.0 - static_cast<double>(largestFreeKB) / freeKB) * 100.0;

    std::ostringstream output;
    output << "=== System Overview ===\n\n";

    // [1] 系统摘要
    output << renderSystemSummary(
        currentUser,
        userProcesses.size(),
        totalMemoryKB,
        usedKB,
        freeKB,
        largestFreeKB,
        fragmentationRate,
        schedulerInfo,
        snapshotPath,
        algorithmName);

    // [2] 进程树
    output << "\n" << renderProcessTree(currentUser, userProcesses);

    // [3] 内存分区图
    output << "\n" << renderMemoryMap(currentUser, memoryBlocks, totalMemoryKB);

    // [4] MLFQ 多级反馈队列
    output << "\n" << renderMLFQ(currentUser, readyQueues, userProcesses);

    // [5] Notes
    output << "\n" << renderNotes();

    return output.str();
}

std::string OverviewRenderer::renderSystemSummary(
    const std::string& currentUser,
    std::size_t processCount,
    std::uint32_t totalMemoryKB,
    std::uint32_t usedMemoryKB,
    std::uint32_t freeMemoryKB,
    std::uint32_t largestFreeKB,
    double fragmentationRate,
    const SchedulerInfo& schedulerInfo,
    const std::string& snapshotPath,
    const std::string& algorithmName) const {
    std::ostringstream output;
    output << "[1] System Summary\n"
           << "Current User: " << (currentUser.empty() ? "<none>" : currentUser) << '\n'
           << "Scheduler: " << (schedulerInfo.running ? "RUNNING" : "STOPPED") << '\n'
           << "Scheduler Owner: " << (schedulerInfo.owner.empty() ? "<none>" : schedulerInfo.owner) << '\n'
           << "Scheduler Interval: " << schedulerInfo.intervalMs << "ms\n"
           << "Allocation Algorithm: " << algorithmName << '\n'
           << "Snapshot File: " << snapshotPath << '\n'
           << '\n'
           << "Total Memory: " << totalMemoryKB << "KB\n"
           << "Used Memory: " << usedMemoryKB << "KB\n"
           << "Free Memory: " << freeMemoryKB << "KB\n"
           << "Largest Free Block: " << largestFreeKB << "KB\n"
           << "External Fragmentation: " << std::fixed << std::setprecision(2) << fragmentationRate << "%\n"
           << "Process Count: " << processCount;
    return output.str();
}

std::string OverviewRenderer::renderProcessTree(
    const std::string& currentUser,
    const std::vector<PCB>& userProcesses) const {
    std::ostringstream output;
    output << "[2] Process Tree\n";

    if (userProcesses.empty()) {
        output << "No process found for current user.";
        return output.str();
    }

    // 构建 PID -> PCB 快速查找表（副本，OverviewRenderer 不持有原表引用）
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    for (const auto& pcb : userProcesses) {
        pcbMap[pcb.pid] = pcb;
    }

    // 找出根进程：ppid==0 或父进程不存在或不属于当前用户
    std::vector<std::uint32_t> roots;
    for (const auto& [pid, pcb] : pcbMap) {
        if (pcb.ppid == 0) {
            roots.push_back(pid);
            continue;
        }
        auto parent = pcbMap.find(pcb.ppid);
        if (parent == pcbMap.end()) {
            // 父进程不属于当前用户，当前进程作为根
            roots.push_back(pid);
        }
    }
    std::sort(roots.begin(), roots.end());

    std::unordered_set<std::uint32_t> visited;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (i > 0) {
            output << '\n';  // 多个根进程之间换行分隔
        }
        appendTreeNode(roots[i], "", i + 1 == roots.size(), true, pcbMap, visited, output);
    }

    // 为保持输出整洁，如果最后一行是换行就去掉
    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

void OverviewRenderer::appendTreeNode(
    std::uint32_t pid,
    const std::string& prefix,
    bool isLast,
    bool isRoot,
    const std::unordered_map<std::uint32_t, PCB>& pcbMap,
    std::unordered_set<std::uint32_t>& visited,
    std::ostringstream& output) const {
    // 防止异常父子关系导致无限递归
    if (visited.find(pid) != visited.end()) {
        output << prefix << (isLast ? "`- " : "+- ")
               << "PID=" << pid << " (cycle detected)";
        return;
    }

    auto it = pcbMap.find(pid);
    if (it == pcbMap.end()) {
        return;
    }

    visited.insert(pid);
    const auto& pcb = it->second;

    // 根节点不加前缀，子节点使用 ASCII 树形前缀
    if (!isRoot) {
        output << prefix << (isLast ? "`- " : "+- ");
    }
    output << nodeText(pcb);

    // 收集属于当前用户的子进程，按 PID 升序
    std::vector<std::uint32_t> visibleChildren;
    for (const auto childPid : pcb.children) {
        if (pcbMap.find(childPid) != pcbMap.end()) {
            visibleChildren.push_back(childPid);
        }
    }
    std::sort(visibleChildren.begin(), visibleChildren.end());

    // 递归渲染子节点
    const std::string childPrefix = isRoot ? "" : prefix + (isLast ? "   " : "|  ");
    for (std::size_t i = 0; i < visibleChildren.size(); ++i) {
        output << '\n';
        appendTreeNode(
            visibleChildren[i],
            childPrefix,
            i + 1 == visibleChildren.size(),
            false,
            pcbMap,
            visited,
            output);
    }
}

std::string OverviewRenderer::nodeText(const PCB& pcb) {
    std::ostringstream text;
    text << pcb.name << '(' << pcb.pid << ") ["
         << toString(pcb.state)
         << ", prio=" << pcb.priority
         << ", q=Q" << pcb.queueLevel
         << ", mem=" << pcb.memSize << "KB"
         << ", cpu=" << pcb.executedTime << '/' << pcb.totalTime
         << ']';
    return text.str();
}

std::string OverviewRenderer::renderMemoryMap(
    const std::string& currentUser,
    const std::vector<MemoryBlock>& memoryBlocks,
    std::uint32_t totalMemoryKB) const {
    std::ostringstream output;
    output << "[3] Memory Map (0-" << totalMemoryKB << "KB)\n";

    if (memoryBlocks.empty()) {
        output << "No memory blocks.";
        return output.str();
    }

    // ASCII 内存条
    for (const auto& block : memoryBlocks) {
        if (block.type == MemoryBlockType::FREE) {
            output << "|--FREE:" << block.size << "KB--";
        } else if (block.type == MemoryBlockType::PROCESS) {
            const bool owned = block.owner == currentUser;
            if (owned) {
                output << "|##P" << block.pid << ':' << block.tag << ':' << block.size << "KB";
            } else {
                output << "|##OTHER_USER:" << block.size << "KB";
            }
        } else if (block.type == MemoryBlockType::KERNEL) {
            const bool owned = block.owner == currentUser;
            if (owned) {
                output << "|##" << block.tag << ':' << block.size << "KB";
            } else {
                output << "|##OTHER_USER:" << block.size << "KB";
            }
        } else if (block.type == MemoryBlockType::SWAPPED) {
            output << "|##SWAPPED:P" << block.pid << ':' << block.size << "KB";
        }
    }
    output << "|\n\n";

    // 内存块明细表
    output << "Memory Blocks:\n"
           << std::left
           << std::setw(8) << "Start"
           << std::setw(8) << "End"
           << std::setw(9) << "SizeKB"
           << std::setw(10) << "Type"
           << std::setw(12) << "Owner"
           << std::setw(7) << "PID"
           << "Tag\n";

    for (const auto& block : memoryBlocks) {
        const bool free = block.type == MemoryBlockType::FREE;
        const bool owned = !free && block.owner == currentUser;
        // 对非当前用户的内存块隐藏细节
        const auto visibleOwner = free ? "-" : (owned ? block.owner : "OTHER_USER");
        const auto visiblePid = free ? "-" : std::to_string(block.pid);
        const auto visibleTag = free ? "-" : block.tag;
        output << std::left
               << std::setw(8) << block.start
               << std::setw(8) << (block.start + block.size - 1)
               << std::setw(9) << block.size
               << std::setw(10) << toString(block.type)
               << std::setw(12) << visibleOwner
               << std::setw(7) << visiblePid
               << visibleTag << '\n';
    }

    // 去掉末尾换行
    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

std::string OverviewRenderer::renderMLFQ(
    const std::string& currentUser,
    const std::array<std::vector<std::uint32_t>, 3>& readyQueues,
    const std::vector<PCB>& userProcesses) const {
    // 构建 PID -> PCB 查找表
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    for (const auto& pcb : userProcesses) {
        pcbMap[pcb.pid] = pcb;
    }

    std::ostringstream output;
    output << "[4] MLFQ\n";

    std::vector<std::string> warnings;

    for (int q = 0; q < 3; ++q) {
        output << 'Q' << q << "(prio " << priorityRangeForQueue(q)
               << ", quantum=" << quantumForQueue(q) << "): ";

        if (readyQueues[q].empty()) {
            output << "empty\n";
            continue;
        }

        bool first = true;
        for (const auto pid : readyQueues[q]) {
            if (!first) {
                output << ", ";
            }
            first = false;

            auto it = pcbMap.find(pid);
            if (it == pcbMap.end()) {
                // PID 不在当前用户的 PCB 表中 — 可能是其他用户的进程或无效 PID
                // 检查该 PID 是否在其他地方有记录（通过是否属于当前用户无法判断）
                output << pid << "(?)";
                warnings.push_back("PID=" + std::to_string(pid)
                    + " appears in Q" + std::to_string(q)
                    + " but does not belong to current user or does not exist.");
                continue;
            }

            const auto& pcb = it->second;
            output << pid << '(' << pcb.name << ')';

            // 检查状态一致性
            if (pcb.state != ProcessState::READY) {
                warnings.push_back("PID=" + std::to_string(pid)
                    + " appears in Q" + std::to_string(q)
                    + " but state is " + toString(pcb.state) + ".");
            }
            // 检查队列层级一致性
            if (pcb.queueLevel != q) {
                warnings.push_back("PID=" + std::to_string(pid)
                    + " appears in Q" + std::to_string(q)
                    + " but PCB queueLevel is Q" + std::to_string(pcb.queueLevel) + ".");
            }
            // 检查 swappedOut
            if (pcb.swappedOut) {
                warnings.push_back("PID=" + std::to_string(pid)
                    + " appears in Q" + std::to_string(q)
                    + " but PCB is swapped out.");
            }
        }
        output << '\n';
    }

    // 输出警告
    if (!warnings.empty()) {
        output << '\n' << "[Warnings]\n";
        for (const auto& warning : warnings) {
            output << "- " << warning << '\n';
        }
    }

    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

std::string OverviewRenderer::renderNotes() {
    return "[5] Notes\n"
           "- BLOCKED, SUSPENDED, SWAPPED, and TERMINATED processes are not schedulable.\n"
           "- Memory map is global. Other users' blocks are read-only.\n"
           "- overview is a read-only snapshot command.\n"
           "- Use step, list_pcb, show_mem, readyq for detailed views.";
}

std::string OverviewRenderer::queueName(int level) {
    return "Q" + std::to_string(level);
}

int OverviewRenderer::quantumForQueue(int level) {
    switch (level) {
    case 0: return 2;
    case 1: return 4;
    default: return 8;
    }
}

std::string OverviewRenderer::priorityRangeForQueue(int level) {
    switch (level) {
    case 0: return "0-3";
    case 1: return "4-7";
    default: return "8-15";
    }
}

} // namespace oscore
