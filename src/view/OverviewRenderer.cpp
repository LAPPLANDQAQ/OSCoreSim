#include "view/OverviewRenderer.h"

#include "util/StringUtil.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace oscore {
namespace {

std::string progressBar(std::uint32_t value, std::uint32_t total, int width) {
    const double ratio = total == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(total);
    const int filled = std::clamp(static_cast<int>(ratio * width + 0.5), 0, width);
    std::string bar; bar.reserve(static_cast<std::size_t>(width) + 2);
    bar.push_back('['); bar.append(static_cast<std::size_t>(filled), '#'); bar.append(static_cast<std::size_t>(width - filled), '.'); bar.push_back(']');
    return bar;
}

double percentOf(std::uint32_t value, std::uint32_t total) { if (total == 0) return 0.0; return static_cast<double>(value) * 100.0 / static_cast<double>(total); }
std::string schedulerStateLabel(bool running) { return running ? "RUNNING" : "STOPPED"; }

} // namespace

std::string OverviewRenderer::render(
    const std::string& currentUser, const std::vector<PCB>& userProcesses,
    const std::array<std::vector<std::uint32_t>, 3>& readyQueues,
    const std::vector<MemoryBlock>& memoryBlocks, std::uint32_t totalMemoryKB,
    const SchedulerInfo& schedulerInfo, const std::string& snapshotPath,
    const std::string& algorithmName, std::size_t vfsFileCount) const {

    std::uint32_t usedKB = 0, freeKB = 0, largestFreeKB = 0;
    for (const auto& block : memoryBlocks) {
        if (block.type == MemoryBlockType::FREE) { freeKB += block.size; largestFreeKB = std::max(largestFreeKB, block.size); }
        else { usedKB += block.size; }
    }
    const double fragmentationRate = freeKB == 0 ? 0.0 : (1.0 - static_cast<double>(largestFreeKB) / freeKB) * 100.0;

    std::ostringstream output;
    output << "==================== OSCoreSim 系统总览 / System Overview ====================\n\n";

    output << "[用户信息 / User]\n";
    output << renderSystemSummary(currentUser, userProcesses.size(), totalMemoryKB, usedKB, freeKB, largestFreeKB,
        fragmentationRate, schedulerInfo, snapshotPath, algorithmName, vfsFileCount);

    output << "\n\n[进程树 / Process Tree]\n";
    output << renderProcessTree(currentUser, userProcesses);

    output << "\n\n[内存布局 / Memory Layout]\n";
    output << renderMemoryMap(currentUser, memoryBlocks, totalMemoryKB);

    output << "\n[就绪队列 / MLFQ Ready Queues]\n";
    output << renderMLFQ(currentUser, readyQueues, userProcesses);

    output << "\n[警告 / Warnings]\n无 / None"
           << "\n=======================================================================";

    return output.str();
}

std::string OverviewRenderer::renderSystemSummary(
    const std::string& currentUser, std::size_t processCount, std::uint32_t totalMemoryKB,
    std::uint32_t usedMemoryKB, std::uint32_t freeMemoryKB, std::uint32_t largestFreeKB,
    double fragmentationRate, const SchedulerInfo& schedulerInfo, const std::string& snapshotPath,
    const std::string& algorithmName, std::size_t vfsFileCount) const {
    const auto user = currentUser.empty() ? "无 / None" : currentUser;
    const auto memoryPercent = percentOf(usedMemoryKB, totalMemoryKB);
    std::ostringstream output;
    output << std::left
           << "当前用户 / Current user : " << user << '\n'
           << "快照文件 / Snapshot     : " << snapshotPath << '\n'
           << "调度器 / Scheduler      : " << schedulerStateLabel(schedulerInfo.running) << '\n'
           << "分配算法 / Algorithm    : " << algorithmName << '\n'
           << "进程数量 / Processes    : " << processCount << '\n'
           << "虚拟文件 / VFS Files    : " << vfsFileCount << '\n'
           << "内存总量 / Total Memory : " << totalMemoryKB << " KB\n"
           << "已用内存 / Used Memory  : " << usedMemoryKB << " KB  "
           << progressBar(usedMemoryKB, totalMemoryKB, 24) << ' ' << std::fixed << std::setprecision(1) << memoryPercent << "%\n"
           << "空闲内存 / Free Memory  : " << freeMemoryKB << " KB\n"
           << "最大空闲块 / Largest Free : " << largestFreeKB << " KB\n"
           << "碎片率 / Fragmentation: " << std::fixed << std::setprecision(1) << fragmentationRate << '%';
    return output.str();
}

std::string OverviewRenderer::renderProcessTable(const std::vector<PCB>& userProcesses) {
    std::ostringstream output;
    output << "Process Table\n";
    if (userProcesses.empty()) { output << "No process found."; return output.str(); }
    auto rows = userProcesses;
    std::sort(rows.begin(), rows.end(), [](const PCB& lhs, const PCB& rhs) { return lhs.pid < rhs.pid; });
    output << std::left
           << padRightDisplayWidth("PID", 6)
           << padRightDisplayWidth("PPID", 6)
           << padRightDisplayWidth("Name", 16)
           << padRightDisplayWidth("State", 14)
           << padRightDisplayWidth("Prio", 6)
           << padRightDisplayWidth("Queue", 6)
           << padRightDisplayWidth("CPU", 11)
           << padRightDisplayWidth("CPU Progress", 20)
           << padRightDisplayWidth("Mem", 8)
           << padRightDisplayWidth("Swap", 8)
           << '\n';
    for (const auto& pcb : rows) {
        const auto cpuText = std::to_string(pcb.executedTime) + "/" + std::to_string(pcb.totalTime);
        const auto queueText = "Q" + std::to_string(pcb.queueLevel);
        output << std::left
               << padRightDisplayWidth(std::to_string(pcb.pid), 6)
               << padRightDisplayWidth(std::to_string(pcb.ppid), 6)
               << padRightDisplayWidth(pcb.name, 16)
               << padRightDisplayWidth(toString(pcb.state), 14)
               << padRightDisplayWidth(std::to_string(pcb.priority), 6)
               << padRightDisplayWidth(queueText, 6)
               << padRightDisplayWidth(cpuText, 11)
               << padRightDisplayWidth(progressBar(pcb.executedTime, pcb.totalTime, 14), 20)
               << padRightDisplayWidth(std::to_string(pcb.memSize), 8)
               << padRightDisplayWidth(pcb.swappedOut ? "是/Yes" : "否/No", 8)
               << '\n';
    }
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string OverviewRenderer::renderProcessTree(const std::string& currentUser, const std::vector<PCB>& userProcesses) const {
    std::ostringstream output;
    output << "Process Tree [User=" << currentUser << "]\n";
    if (userProcesses.empty()) { output << "No process found."; return output.str(); }
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    for (const auto& pcb : userProcesses) { pcbMap[pcb.pid] = pcb; }
    std::vector<std::uint32_t> roots;
    for (const auto& [pid, pcb] : pcbMap) {
        if (pcb.ppid == 0) { roots.push_back(pid); continue; }
        auto parent = pcbMap.find(pcb.ppid);
        if (parent == pcbMap.end()) { roots.push_back(pid); }
    }
    std::sort(roots.begin(), roots.end());
    std::unordered_set<std::uint32_t> visited;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (i > 0) output << '\n';
        appendTreeNode(roots[i], "", i + 1 == roots.size(), true, pcbMap, visited, output);
    }
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

void OverviewRenderer::appendTreeNode(std::uint32_t pid, const std::string& prefix, bool isLast, bool isRoot,
    const std::unordered_map<std::uint32_t, PCB>& pcbMap, std::unordered_set<std::uint32_t>& visited,
    std::ostringstream& output) const {
    if (visited.find(pid) != visited.end()) { output << prefix << (isLast ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ") << "PID=" << pid << " (cycle)"; return; }
    auto it = pcbMap.find(pid); if (it == pcbMap.end()) return;
    visited.insert(pid); const auto& pcb = it->second;
    if (!isRoot) output << prefix << (isLast ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");
    output << nodeText(pcb);
    std::vector<std::uint32_t> visibleChildren;
    for (const auto childPid : pcb.children) { if (pcbMap.find(childPid) != pcbMap.end()) visibleChildren.push_back(childPid); }
    std::sort(visibleChildren.begin(), visibleChildren.end());
    const std::string childPrefix = isRoot ? "" : prefix + (isLast ? "   " : "\xe2\x94\x82  ");
    for (std::size_t i = 0; i < visibleChildren.size(); ++i) {
        output << '\n';
        appendTreeNode(visibleChildren[i], childPrefix, i + 1 == visibleChildren.size(), false, pcbMap, visited, output);
    }
}

std::string OverviewRenderer::nodeText(const PCB& pcb) {
    std::ostringstream text;
    text << std::left
         << padRightDisplayWidth(pcb.name + "(" + std::to_string(pcb.pid) + ")", 24)
         << ' ' << padRightDisplayWidth(toString(pcb.state), 14)
         << " Prio=" << std::right << std::setw(2) << pcb.priority
         << "  Q" << pcb.queueLevel
         << "  CPU=" << std::setw(3) << pcb.executedTime << '/' << std::setw(3) << pcb.totalTime;
    if (pcb.swappedOut) text << "  SWAPPED";
    else text << "  Mem=" << std::setw(3) << pcb.memStart << '+' << std::setw(3) << pcb.memSize << "KB";
    return text.str();
}

std::string OverviewRenderer::renderMemoryMap(const std::string& currentUser, const std::vector<MemoryBlock>& memoryBlocks, std::uint32_t totalMemoryKB) const {
    std::ostringstream output;
    if (memoryBlocks.empty()) { output << "No memory blocks."; return output.str(); }
    std::uint32_t usedKB = 0;
    for (const auto& block : memoryBlocks) { if (block.type != MemoryBlockType::FREE) usedKB += block.size; }
    output << "Address Range: 0-" << totalMemoryKB << " KB\n"
           << "Usage: " << progressBar(usedKB, totalMemoryKB, 32) << ' ' << usedKB << '/' << totalMemoryKB << "KB (" << std::fixed << std::setprecision(2) << percentOf(usedKB, totalMemoryKB) << "%)\n\n";

    const int mapWidth = 64;
    std::string mapLine; mapLine.reserve(static_cast<std::size_t>(mapWidth) + 2);
    for (const auto& block : memoryBlocks) {
        const int blockChars = std::max(1, static_cast<int>(static_cast<double>(block.size) / totalMemoryKB * mapWidth));
        if (block.type == MemoryBlockType::FREE) mapLine.append(static_cast<std::size_t>(blockChars), '.');
        else if (block.type == MemoryBlockType::PROCESS) mapLine.append(static_cast<std::size_t>(blockChars), 'P');
        else mapLine.append(static_cast<std::size_t>(blockChars), 'K');
    }
    if (mapLine.size() > static_cast<std::size_t>(mapWidth)) mapLine.resize(static_cast<std::size_t>(mapWidth));
    while (mapLine.size() < static_cast<std::size_t>(mapWidth)) mapLine.push_back('.');
    output << "Memory Map:\n[" << mapLine << "]\n"
           << "Legend: P=Process, K=Kernel/Manual, .=Free\n\n";

    output << "Memory Blocks:\n" << std::left
           << padRightDisplayWidth("Address", 18)
           << padRightDisplayWidth("Size", 10)
           << padRightDisplayWidth("Type", 10)
           << padRightDisplayWidth("Owner", 12)
           << padRightDisplayWidth("PID", 6) << "Tag\n" << std::string(62, '-') << '\n';
    for (const auto& block : memoryBlocks) {
        const bool free = block.type == MemoryBlockType::FREE;
        const bool owned = !free && block.owner == currentUser;
        const auto visibleOwner = free ? "-" : (owned ? block.owner : "OTHER_USER");
        std::ostringstream addrRange; addrRange << std::setfill('0') << std::setw(4) << block.start << " - " << std::setfill('0') << std::setw(4) << (block.start + block.size - 1) << " KB";
        std::ostringstream sizeStr; sizeStr << block.size << " KB";
        output << std::left << padRightDisplayWidth(addrRange.str(), 18) << padRightDisplayWidth(sizeStr.str(), 10)
               << padRightDisplayWidth(free ? "Free" : toString(block.type), 10)
               << padRightDisplayWidth(visibleOwner, 12) << padRightDisplayWidth(free ? "-" : std::to_string(block.pid), 6)
               << (free ? "-" : block.tag) << '\n';
    }
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string OverviewRenderer::renderMLFQ(const std::string& currentUser, const std::array<std::vector<std::uint32_t>, 3>& readyQueues, const std::vector<PCB>& userProcesses) const {
    (void)currentUser;
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    for (const auto& pcb : userProcesses) { pcbMap[pcb.pid] = pcb; }
    std::ostringstream output; std::vector<std::string> warnings;
    for (int q = 0; q < 3; ++q) {
        output << queueName(q)
               << " | 优先级/Priority: " << priorityRangeForQueue(q)
               << " | 时间片/Quantum: " << quantumForQueue(q) << '\n'
               << "  队列/Queue: ";
        if (readyQueues[q].empty()) { output << "空/empty\n"; continue; }
        bool first = true;
        for (const auto pid : readyQueues[q]) {
            if (!first) output << ", "; first = false;
            auto it = pcbMap.find(pid);
            if (it == pcbMap.end()) {
                output << pid << "(?)";
                warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但不属于当前用户或不存在。");
                continue;
            }
            const auto& pcb = it->second;
            output << "PID=" << pid << '(' << pcb.name << ", 状态=" << toString(pcb.state) << ", cpu=" << pcb.executedTime << '/' << pcb.totalTime << ' ' << progressBar(pcb.executedTime, pcb.totalTime, 8) << ')';
            if (pcb.state != ProcessState::READY) warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但状态为 " + toString(pcb.state) + "。");
            if (pcb.queueLevel != q) warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但 PCB queueLevel 为 Q" + std::to_string(pcb.queueLevel) + "。");
            if (pcb.swappedOut) warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但 PCB 已换出。");
        }
        output << '\n';
    }
    if (!warnings.empty()) { output << '\n' << "[警告]\n"; for (const auto& w : warnings) output << "- " << w << '\n'; }
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string OverviewRenderer::renderNotes() {
    return "说明 / Notes\n"
           "- overview 是只读快照命令，不修改系统状态。\n"
           "- BLOCKED/SUSPENDED/SWAPPED/TERMINATED 进程不会进入可调度队列。\n"
           "- 其他用户的内存块显示为 OTHER_USER，避免泄露细节。\n"
           "- 虚拟文件数量已汇总在 User 区，文件内容请使用 ls_file/read_file/write_file 查看。";
}

std::string OverviewRenderer::queueName(int level) { return "Q" + std::to_string(level); }
int OverviewRenderer::quantumForQueue(int level) { switch (level) { case 0: return 2; case 1: return 4; default: return 8; } }
std::string OverviewRenderer::priorityRangeForQueue(int level) { switch (level) { case 0: return "0-3"; case 1: return "4-7"; default: return "8-15"; } }

} // namespace oscore
