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
    // 按 value/total 比例生成固定宽度进度条，用于 CPU 和内存占用显示。
    const double ratio = total == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(total);
    const int filled = std::clamp(static_cast<int>(ratio * width + 0.5), 0, width);
    std::string bar; bar.reserve(static_cast<std::size_t>(width) + 2);
    bar.push_back('['); bar.append(static_cast<std::size_t>(filled), '#'); bar.append(static_cast<std::size_t>(width - filled), '.'); bar.push_back(']');
    return bar;
}

// 计算百分比，total 为 0 时返回 0，避免除零。
double percentOf(std::uint32_t value, std::uint32_t total) { if (total == 0) return 0.0; return static_cast<double>(value) * 100.0 / static_cast<double>(total); }
// 将调度器布尔状态转换为 overview 显示标签。
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
        // 统计 overview 顶部摘要需要的已用、空闲和最大空闲块。
        if (block.type == MemoryBlockType::FREE) { freeKB += block.size; largestFreeKB = std::max(largestFreeKB, block.size); }
        else { usedKB += block.size; }
    }
    // 外部碎片率沿用 MemoryManager 的公式：1 - 最大空闲块 / 总空闲。
    const double fragmentationRate = freeKB == 0 ? 0.0 : (1.0 - static_cast<double>(largestFreeKB) / freeKB) * 100.0;

    std::ostringstream output;
    // overview 是课程验收的一屏总览入口，按模块顺序拼接多个只读渲染区。
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

    // 返回完整字符串，由 Kernel/ConsoleApp 打印。
    return output.str();
}

std::string OverviewRenderer::renderSystemSummary(
    const std::string& currentUser, std::size_t processCount, std::uint32_t totalMemoryKB,
    std::uint32_t usedMemoryKB, std::uint32_t freeMemoryKB, std::uint32_t largestFreeKB,
    double fragmentationRate, const SchedulerInfo& schedulerInfo, const std::string& snapshotPath,
    const std::string& algorithmName, std::size_t vfsFileCount) const {
    // 未登录时显示“无 / None”，避免空字符串让表格看起来缺字段。
    const auto user = currentUser.empty() ? "无 / None" : currentUser;
    // memoryPercent 用于摘要中的内存进度条。
    const auto memoryPercent = percentOf(usedMemoryKB, totalMemoryKB);
    std::ostringstream output;
    // 系统摘要将用户、调度器、内存、VFS、快照路径放在同一区域，便于验收时快速确认状态。
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
    // 没有进程时直接输出提示，不渲染空表头。
    if (userProcesses.empty()) { output << "No process found."; return output.str(); }
    // 复制并排序，避免改变调用方传入的快照顺序。
    auto rows = userProcesses;
    std::sort(rows.begin(), rows.end(), [](const PCB& lhs, const PCB& rhs) { return lhs.pid < rhs.pid; });
    // 表头使用显示宽度对齐，兼容中英文混排。
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
        // CPU 列和进度条都由 PCB 的 executedTime/totalTime 计算。
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
    // 去掉末尾换行，便于上层继续拼接其他区块。
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string OverviewRenderer::renderProcessTree(const std::string& currentUser, const std::vector<PCB>& userProcesses) const {
    std::ostringstream output;
    output << "Process Tree [User=" << currentUser << "]\n";
    // 当前用户没有进程时直接提示。
    if (userProcesses.empty()) { output << "No process found."; return output.str(); }
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    // 构建 PID 到 PCB 的查找表，递归渲染时不用反复扫描 vector。
    for (const auto& pcb : userProcesses) { pcbMap[pcb.pid] = pcb; }
    std::vector<std::uint32_t> roots;
    for (const auto& [pid, pcb] : pcbMap) {
        // ppid=0 的进程是根节点。
        if (pcb.ppid == 0) { roots.push_back(pid); continue; }
        auto parent = pcbMap.find(pcb.ppid);
        // 父进程不在当前用户快照中时，也把该节点作为根显示，避免丢失孤儿节点。
        if (parent == pcbMap.end()) { roots.push_back(pid); }
    }
    // 根节点按 PID 排序，保证输出稳定。
    std::sort(roots.begin(), roots.end());
    std::unordered_set<std::uint32_t> visited;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        // 多个根节点之间空一行。
        if (i > 0) output << '\n';
        appendTreeNode(roots[i], "", i + 1 == roots.size(), true, pcbMap, visited, output);
    }
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

void OverviewRenderer::appendTreeNode(std::uint32_t pid, const std::string& prefix, bool isLast, bool isRoot,
    const std::unordered_map<std::uint32_t, PCB>& pcbMap, std::unordered_set<std::uint32_t>& visited,
    std::ostringstream& output) const {
    // visited 防止异常父子关系形成环时无限递归。
    if (visited.find(pid) != visited.end()) { output << prefix << (isLast ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ") << "PID=" << pid << " (cycle)"; return; }
    auto it = pcbMap.find(pid); if (it == pcbMap.end()) return;
    visited.insert(pid); const auto& pcb = it->second;
    // 根节点不显示树枝前缀，子节点显示 └─ 或 ├─。
    if (!isRoot) output << prefix << (isLast ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");
    output << nodeText(pcb);
    std::vector<std::uint32_t> visibleChildren;
    // 只渲染当前快照中存在的子进程。
    for (const auto childPid : pcb.children) { if (pcbMap.find(childPid) != pcbMap.end()) visibleChildren.push_back(childPid); }
    // 子节点按 PID 排序，保证树结构稳定。
    std::sort(visibleChildren.begin(), visibleChildren.end());
    // 子节点前缀根据当前节点是否是最后一个兄弟节点决定。
    const std::string childPrefix = isRoot ? "" : prefix + (isLast ? "   " : "\xe2\x94\x82  ");
    for (std::size_t i = 0; i < visibleChildren.size(); ++i) {
        output << '\n';
        appendTreeNode(visibleChildren[i], childPrefix, i + 1 == visibleChildren.size(), false, pcbMap, visited, output);
    }
}

std::string OverviewRenderer::nodeText(const PCB& pcb) {
    std::ostringstream text;
    // 进程树中每个节点的单行格式化输出，仅影响显示排版，不修改 PCB 数据或进程状态。
    text << std::left
         // Name 列 18 宽度让进程树行更紧凑；State 列 18 宽度避免 SUSPENDED_READY / SUSPENDED_BLOCKED 等长状态名溢出。
         << padRightDisplayWidth(pcb.name + "(" + std::to_string(pcb.pid) + ")", 18)
         << ' ' << padRightDisplayWidth(toString(pcb.state), 18)
         << " Prio=" << std::right << std::setw(2) << pcb.priority
         << "  Q" << pcb.queueLevel
         << "  CPU=" << std::setw(3) << pcb.executedTime << '/' << std::setw(3) << pcb.totalTime;
    if (pcb.swappedOut) text << "  SWAPPED";
    else text << "  Mem=" << std::setw(3) << pcb.memStart << '+' << std::setw(3) << pcb.memSize << "KB";
    return text.str();
}

std::string OverviewRenderer::renderMemoryMap(const std::string& currentUser, const std::vector<MemoryBlock>& memoryBlocks, std::uint32_t totalMemoryKB) const {
    std::ostringstream output;
    // 内存块为空时直接提示，避免后续按比例计算除零。
    if (memoryBlocks.empty()) { output << "No memory blocks."; return output.str(); }
    std::uint32_t usedKB = 0;
    // 统计所有非 FREE 块作为已用内存。
    for (const auto& block : memoryBlocks) { if (block.type != MemoryBlockType::FREE) usedKB += block.size; }
    output << "Address Range: 0-" << totalMemoryKB << " KB\n"
           << "Usage: " << progressBar(usedKB, totalMemoryKB, 32) << ' ' << usedKB << '/' << totalMemoryKB << "KB (" << std::fixed << std::setprecision(2) << percentOf(usedKB, totalMemoryKB) << "%)\n\n";

    // Memory Map 使用竖线分隔的块列表替代 ASCII 比例条，每个段对应一个物理内存块。
    // 竖线仅作为可视化分隔符，使控制台输出和演示截图中内存分区边界更加清晰。
    // 本段只改变展示格式，不修改内存块数据、分配状态或分配算法。
    output << "Memory Map:\n";
    int lineLen = 0;
    bool first = true;
    for (const auto& block : memoryBlocks) {
        std::ostringstream entry;
        std::string label = (block.type == MemoryBlockType::PROCESS) ? block.tag
                          : (block.type == MemoryBlockType::KERNEL) ? block.tag : "";
        if (block.type == MemoryBlockType::FREE)
            entry << "Free(" << std::setfill('0') << std::setw(4) << block.size << ")";
        else if (block.type == MemoryBlockType::PROCESS)
            entry << "P-" << label << "(" << std::setfill('0') << std::setw(4) << block.start << "," << block.size << ")";
        else
            entry << "K-" << label << "(" << std::setfill('0') << std::setw(4) << block.start << "," << block.size << ")";
        std::string seg = (first ? "" : " | ") + entry.str();
        if (lineLen > 0 && lineLen + static_cast<int>(seg.size()) > 76) { output << '\n'; lineLen = 0; seg = entry.str(); }
        output << seg;
        lineLen += static_cast<int>(seg.size());
        first = false;
    }
    output << "\nLegend: P=Process, K=Kernel/Manual, Free=空闲\n\n";

    output << "Memory Blocks:\n" << std::left
           << padRightDisplayWidth("Address", 18)
           << padRightDisplayWidth("Size", 10)
           << padRightDisplayWidth("Type", 10)
           << padRightDisplayWidth("Owner", 12)
           << padRightDisplayWidth("PID", 6) << "Tag\n" << std::string(62, '-') << '\n';
    for (const auto& block : memoryBlocks) {
        const bool free = block.type == MemoryBlockType::FREE;
        // 非当前用户的已分配块隐藏 owner，避免 overview 泄露多用户细节。
        const bool owned = !free && block.owner == currentUser;
        const auto visibleOwner = free ? "-" : (owned ? block.owner : "OTHER_USER");
        // 地址范围展示 start 到 start+size-1。
        std::ostringstream addrRange; addrRange << std::setfill('0') << std::setw(4) << block.start << " - " << std::setfill('0') << std::setw(4) << (block.start + block.size - 1) << " KB";
        std::ostringstream sizeStr; sizeStr << block.size << " KB";
        output << std::left << padRightDisplayWidth(addrRange.str(), 18) << padRightDisplayWidth(sizeStr.str(), 10)
               << padRightDisplayWidth(free ? "Free" : toString(block.type), 10)
               << padRightDisplayWidth(visibleOwner, 12) << padRightDisplayWidth(free ? "-" : std::to_string(block.pid), 6)
               << (free ? "-" : block.tag) << '\n';
    }
    // 去掉末尾换行，保持区块拼接紧凑。
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string OverviewRenderer::renderMLFQ(const std::string& currentUser, const std::array<std::vector<std::uint32_t>, 3>& readyQueues, const std::vector<PCB>& userProcesses) const {
    // currentUser 当前只用于接口一致性，队列检查通过 userProcesses 快照完成。
    (void)currentUser;
    std::unordered_map<std::uint32_t, PCB> pcbMap;
    // 建立 PID → PCB 映射，便于检查队列中的 PID 是否有效。
    for (const auto& pcb : userProcesses) { pcbMap[pcb.pid] = pcb; }
    std::ostringstream output; std::vector<std::string> warnings;
    for (int q = 0; q < 3; ++q) {
        // 紧凑格式：Q0/Q1/Q2 为三级 MLFQ 队列，prio 标注优先级范围，Quan 标注时间片大小。
        // 每个队列一行展示全部就绪进程，便于 overview 在有限行数内呈现完整调度状态。
        // 仅修改显示排版，不影响调度器 step 逻辑、降级规则和 timeSliceLeft 计算。
        output << queueName(q) << "(prio:" << priorityRangeForQueue(q) << ", Quan:" << quantumForQueue(q) << "): ";
        if (readyQueues[q].empty()) { output << "empty\n"; continue; }
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
            output << pcb.name << "(" << pid << ")";
            if (pcb.state != ProcessState::READY) warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但状态为 " + toString(pcb.state) + "。");
            if (pcb.queueLevel != q) warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但 PCB queueLevel 为 Q" + std::to_string(pcb.queueLevel) + "。");
            if (pcb.swappedOut) warnings.push_back("PID=" + std::to_string(pid) + " 出现在 Q" + std::to_string(q) + " 但 PCB 已换出。");
        }
        output << '\n';
    }
    if (!warnings.empty()) { output << '\n' << "[Warnings]\n"; for (const auto& w : warnings) output << "- " << w << '\n'; }
    std::string result = output.str(); if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string OverviewRenderer::renderNotes() {
    // 说明文本保留为静态辅助函数，供后续 overview 扩展使用。
    return "说明 / Notes\n"
           "- overview 是只读快照命令，不修改系统状态。\n"
           "- BLOCKED/SUSPENDED/SWAPPED/TERMINATED 进程不会进入可调度队列。\n"
           "- 其他用户的内存块显示为 OTHER_USER，避免泄露细节。\n"
           "- 虚拟文件数量已汇总在 User 区，文件内容请使用 ls_file/read_file/write_file 查看。";
}

// 队列名称统一为 Q0/Q1/Q2。
std::string OverviewRenderer::queueName(int level) { return "Q" + std::to_string(level); }
// overview 中展示的时间片必须与 Scheduler/ProcessManager 保持一致。
int OverviewRenderer::quantumForQueue(int level) { switch (level) { case 0: return 2; case 1: return 4; default: return 8; } }
// overview 中展示的优先级范围必须与 ProcessManager::queueLevelForPriority 保持一致。
std::string OverviewRenderer::priorityRangeForQueue(int level) { switch (level) { case 0: return "0-3"; case 1: return "4-7"; default: return "8-15"; } }

} // namespace oscore
