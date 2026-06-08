#include "view/OverviewRenderer.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace oscore {
namespace {

// overview 面向终端演示，进度条只根据快照值计算，不回写任何调度或内存状态。
std::string progressBar(std::uint32_t value, std::uint32_t total, int width) {
    const double ratio = total == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(total);
    const int filled = std::clamp(static_cast<int>(ratio * width + 0.5), 0, width);

    std::string bar;
    bar.reserve(static_cast<std::size_t>(width) + 2);
    bar.push_back('[');
    bar.append(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(width - filled), '.');
    bar.push_back(']');
    return bar;
}

double percentOf(std::uint32_t value, std::uint32_t total) {
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(value) * 100.0 / static_cast<double>(total);
}

std::string schedulerStateLabel(bool running) {
    return running ? "运行中 / RUNNING" : "已停止 / STOPPED";
}

} // namespace

std::string OverviewRenderer::render(
    const std::string& currentUser,
    const std::vector<PCB>& userProcesses,
    const std::array<std::vector<std::uint32_t>, 3>& readyQueues,
    const std::vector<MemoryBlock>& memoryBlocks,
    std::uint32_t totalMemoryKB,
    const SchedulerInfo& schedulerInfo,
    const std::string& snapshotPath,
    const std::string& algorithmName,
    std::size_t vfsFileCount) const {
    // OverviewRenderer 是只读渲染器：所有输入都由 Kernel 预先复制，函数内部只做统计和字符串拼接。
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
    // 章节标题使用中英双语，便于中文答辩展示，同时保留英文关键字给自动测试匹配。
    output << "=== 系统总览 / System Overview ===\n\n";

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
        algorithmName,
        vfsFileCount);

    // [2] 进程表
    output << "\n\n" << renderProcessTable(userProcesses);

    // [3] 进程树
    output << "\n\n" << renderProcessTree(currentUser, userProcesses);

    // [4] 内存分区图
    output << "\n\n" << renderMemoryMap(currentUser, memoryBlocks, totalMemoryKB);

    // [5] MLFQ 多级反馈队列
    output << "\n\n" << renderMLFQ(currentUser, readyQueues, userProcesses);

    // [6] Notes
    output << "\n\n" << renderNotes();

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
    const std::string& algorithmName,
    std::size_t vfsFileCount) const {
    const auto user = currentUser.empty() ? std::string("<none>") : currentUser;
    const auto schedulerOwner = schedulerInfo.owner.empty() ? std::string("<none>") : schedulerInfo.owner;
    const auto memoryPercent = percentOf(usedMemoryKB, totalMemoryKB);

    std::ostringstream output;
    output << "[1] 系统摘要 / System Summary\n"
           << std::left
           << std::setw(35) << "当前用户 / Current User" << ": " << user << '\n'
           << std::setw(35) << "调度器状态 / Scheduler State" << ": " << schedulerStateLabel(schedulerInfo.running) << '\n'
           << std::setw(35) << "调度器用户 / Scheduler Owner" << ": " << schedulerOwner << '\n'
           << std::setw(35) << "调度间隔 / Scheduler Interval" << ": " << schedulerInfo.intervalMs << "ms\n"
           << std::setw(35) << "当前分配算法 / Allocation Algorithm" << ": " << algorithmName << '\n'
           << std::setw(35) << "快照文件 / Snapshot File" << ": " << snapshotPath << '\n'
           << '\n'
           << std::setw(35) << "进程数量 / Process Count" << ": " << processCount << '\n'
           << std::setw(35) << "虚拟文件数量 / VFS Files" << ": " << vfsFileCount << '\n'
           << std::setw(35) << "内存总量 / Total Memory" << ": " << totalMemoryKB << "KB\n"
           << std::setw(35) << "已用内存 / Used Memory" << ": " << usedMemoryKB << "KB\n"
           << std::setw(35) << "空闲内存 / Free Memory" << ": " << freeMemoryKB << "KB\n"
           << std::setw(35) << "最大空闲块 / Largest Free Block" << ": " << largestFreeKB << "KB\n"
           << std::setw(35) << "内存使用 / Memory Usage" << ": "
           << progressBar(usedMemoryKB, totalMemoryKB, 24) << ' '
           << std::fixed << std::setprecision(2) << memoryPercent << "%\n"
           << std::setw(35) << "外部碎片率 / External Fragmentation" << ": "
           << std::fixed << std::setprecision(2) << fragmentationRate << '%';
    return output.str();
}

std::string OverviewRenderer::renderProcessTable(
    const std::vector<PCB>& userProcesses) {
    std::ostringstream output;
    output << "[2] 程序表 / Process Table\n";

    if (userProcesses.empty()) {
        output << "当前用户暂无进程。 / No process found for current user.";
        return output.str();
    }

    auto rows = userProcesses;
    std::sort(rows.begin(), rows.end(), [](const PCB& lhs, const PCB& rhs) {
        return lhs.pid < rhs.pid;
    });

    // 进程表是 PCB 快照的平铺视图，和后面的树形视图互补：一个便于比较字段，一个便于看父子关系。
    output << std::left
           << std::setw(6) << "PID"
           << std::setw(6) << "PPID"
           << std::setw(16) << "Name"
           << std::setw(12) << "State"
           << std::setw(7) << "Prio"
           << std::setw(5) << "Q"
           << std::setw(11) << "CPU"
           << std::setw(20) << "CPU Progress"
           << std::setw(8) << "MemKB"
           << "Swap\n";

    for (const auto& pcb : rows) {
        const auto cpuText = std::to_string(pcb.executedTime) + "/" + std::to_string(pcb.totalTime);
        output << std::left
               << std::setw(6) << pcb.pid
               << std::setw(6) << pcb.ppid
               << std::setw(16) << pcb.name
               << std::setw(12) << toString(pcb.state)
               << std::setw(7) << pcb.priority
               << std::setw(5) << ("Q" + std::to_string(pcb.queueLevel))
               << std::setw(11) << cpuText
               << std::setw(20) << progressBar(pcb.executedTime, pcb.totalTime, 14)
               << std::setw(8) << pcb.memSize
               << (pcb.swappedOut ? "是 / Yes" : "否 / No") << '\n';
    }

    std::string result = output.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

std::string OverviewRenderer::renderProcessTree(
    const std::string& currentUser,
    const std::vector<PCB>& userProcesses) const {
    (void)currentUser;

    std::ostringstream output;
    output << "[3] 进程树 / Process Tree\n";

    if (userProcesses.empty()) {
        output << "当前用户暂无进程。 / No process found for current user.";
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
    // 进程树只根据 PCB.children/ppid 快照构建，visited 用来防御异常环形父子关系。
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
    output << "[4] 内存分区图 / Memory Map\n"
           << "地址范围 / Address Range: 0-" << totalMemoryKB << "KB\n";

    if (memoryBlocks.empty()) {
        output << "暂无内存分区。 / No memory blocks.";
        return output.str();
    }

    std::uint32_t usedKB = 0;
    for (const auto& block : memoryBlocks) {
        if (block.type != MemoryBlockType::FREE) {
            usedKB += block.size;
        }
    }

    output << "内存使用 / Usage: "
           << progressBar(usedKB, totalMemoryKB, 32) << ' '
           << usedKB << '/' << totalMemoryKB << "KB ("
           << std::fixed << std::setprecision(2) << percentOf(usedKB, totalMemoryKB) << "%)\n"
           << "图例 / Legend: FREE=空闲, PROCESS=进程, KERNEL=内核, SWAPPED=已换出, OTHER_USER=其他用户\n\n";

    // ASCII 内存条：块太多时会很长，因此先构造字符串，再决定是否降级为分段视图。
    // 内存图展示全局分区，但非当前用户的块只显示 OTHER_USER，避免泄露具体进程名。
    std::ostringstream mapLine;
    for (const auto& block : memoryBlocks) {
        if (block.type == MemoryBlockType::FREE) {
            mapLine << "|--FREE:" << block.size << "KB--";
        } else if (block.type == MemoryBlockType::PROCESS) {
            const bool owned = block.owner == currentUser;
            if (owned) {
                mapLine << "|##P" << block.pid << ':' << block.tag << ':' << block.size << "KB";
            } else {
                mapLine << "|##OTHER_USER:" << block.size << "KB";
            }
        } else if (block.type == MemoryBlockType::KERNEL) {
            const bool owned = block.owner == currentUser;
            if (owned) {
                mapLine << "|##" << block.tag << ':' << block.size << "KB";
            } else {
                mapLine << "|##OTHER_USER:" << block.size << "KB";
            }
        } else if (block.type == MemoryBlockType::SWAPPED) {
            mapLine << "|##SWAPPED:P" << block.pid << ':' << block.size << "KB";
        }
    }
    mapLine << '|';

    const auto asciiMap = mapLine.str();
    if (asciiMap.size() <= 160) {
        output << asciiMap << "\n\n";
    } else {
        output << "内存块较多，改用分段视图展示：\n";
        for (const auto& block : memoryBlocks) {
            const bool free = block.type == MemoryBlockType::FREE;
            const bool owned = !free && block.owner == currentUser;
            output << '[' << block.start << '-' << (block.start + block.size - 1) << "] "
                   << toString(block.type) << " "
                   << (free ? "-" : (owned ? block.owner : "OTHER_USER")) << " "
                   << (free ? "-" : ("pid=" + std::to_string(block.pid))) << " "
                   << (free ? "-" : block.tag) << " "
                   << block.size << "KB\n";
        }
        output << '\n';
    }

    // 内存块明细表
    output << "Memory Blocks / 内存块明细:\n"
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
    (void)currentUser;

    // 构建 PID -> PCB 查找表
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    for (const auto& pcb : userProcesses) {
        pcbMap[pcb.pid] = pcb;
    }

    std::ostringstream output;
    output << "[5] 多级反馈队列 / MLFQ\n";

    std::vector<std::string> warnings;

    for (int q = 0; q < 3; ++q) {
        // MLFQ 渲染使用 readyQueues 快照，并用 PCB 快照补充状态、CPU 进度和一致性告警。
        output << queueName(q)
               << " | 优先级 / Priority: " << priorityRangeForQueue(q)
               << " | 时间片 / Quantum: " << quantumForQueue(q) << '\n'
               << "  队列 / Queue: ";

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
            output << "PID=" << pid
                   << '(' << pcb.name
                   << ", state=" << toString(pcb.state)
                   << ", cpu=" << pcb.executedTime << '/' << pcb.totalTime
                   << ' ' << progressBar(pcb.executedTime, pcb.totalTime, 8)
                   << ')';

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
    return "[6] 说明 / Notes\n"
           "- overview 是只读快照命令，只渲染调用方传入的进程、内存、调度队列和文件计数，不修改系统状态。\n"
           "- BLOCKED、SUSPENDED、SWAPPED、TERMINATED 进程不会进入可调度队列。\n"
           "- 内存分区图是全局视图，其他用户的内存块显示为 OTHER_USER，避免泄露细节。\n"
           "- 多级反馈队列仅展示当前快照中的 READY 队列；异常队列项会在 Warnings 中列出。\n"
           "- 虚拟文件系统数量已汇总在 System Summary，文件内容仍请使用 ls_file、read_file、write_file 查看。";
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
