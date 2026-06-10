#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oscore {

// 进程状态枚举：涵盖从创建到终止的完整生命周期。
// NEW           = 刚创建，尚未进入就绪队列
// READY         = 可被调度执行
// RUNNING       = 当前正在 CPU 上执行
// BLOCKED       = 等待事件（如 I/O），不可调度
// SUSPENDED_READY  = 被挂起的 READY 进程，恢复后回到 READY
// SUSPENDED_BLOCKED = 被挂起的 BLOCKED 进程，恢复后回到 BLOCKED
// TERMINATED    = 已结束，等待清理
// SWAPPED       = 物理内存已换出，PCB 仍保留元数据
enum class ProcessState {
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    SUSPENDED_READY,
    SUSPENDED_BLOCKED,
    TERMINATED,
    SWAPPED
};

// PCB（Process Control Block）：进程控制块。
// 保存调度所需的全部元数据、父子关系、内存占用和换出状态。
// 该结构保持为简单字段（POD-like），便于 SnapshotStore 显式二进制序列化，
// 避免直接 dump 对象内存导致 ABI/指针/padding 不稳定。
struct PCB {
    std::uint32_t pid = 0;           // 全局唯一进程 ID，递增不重用
    std::uint32_t ppid = 0;          // 父进程 PID，0 表示无父进程（根进程）
    std::string name;                // 进程名称，由用户创建时指定
    std::string owner;               // 所属用户名，用于用户级进程隔离
    ProcessState state = ProcessState::NEW;  // 当前状态（8 种之一）
    int priority = 0;                // 优先级 0-15，映射至 3 级 MLFQ 队列
    int queueLevel = 0;              // 所属 MLFQ 队列层级：Q0(0-3) / Q1(4-7) / Q2(8-15)
    std::uint32_t totalTime = 0;     // 进程总运行时间（tick 数）
    std::uint32_t executedTime = 0;  // 已执行时间
    std::uint32_t remainingTime = 0; // 剩余时间 = totalTime - executedTime
    std::uint32_t timeSliceLeft = 0; // 当前时间片剩余 tick 数（Q0=2, Q1=4, Q2=8）
    std::uint32_t memStart = 0;      // 物理内存起始地址（KB 偏移）
    std::uint32_t memSize = 0;       // 物理内存大小（KB）
    bool swappedOut = false;         // 是否已被换出到交换空间
    std::vector<std::uint32_t> children;  // 子进程 PID 列表，用于进程树展示和递归删除
};

// 将 ProcessState 枚举转为可读字符串，用于终端显示和日志输出。
[[nodiscard]] inline const char* toString(ProcessState state) {
    // 统一把状态枚举转为英文状态名，供列表、详情、树形输出复用。
    switch (state) {
    case ProcessState::NEW:              return "NEW";
    case ProcessState::READY:            return "READY";
    case ProcessState::RUNNING:          return "RUNNING";
    case ProcessState::BLOCKED:          return "BLOCKED";
    case ProcessState::SUSPENDED_READY:  return "SUSPENDED_READY";
    case ProcessState::SUSPENDED_BLOCKED: return "SUSPENDED_BLOCKED";
    case ProcessState::TERMINATED:       return "TERMINATED";
    case ProcessState::SWAPPED:          return "SWAPPED";
    }
    // 理论上不会到达；UNKNOWN 用于兜底显示异常状态。
    return "UNKNOWN";
}

} // namespace oscore
