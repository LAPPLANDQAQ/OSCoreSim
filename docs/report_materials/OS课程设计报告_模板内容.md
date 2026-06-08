# 可持久化操作系统核心模拟器设计与实现

> **说明**：本文档为课程设计报告模板，各章节已填入简要内容框架，
> 请根据实际实现细节和截图补充完善。

---

## 1. 设计目标

本项目旨在实现一个命令行操作系统核心模拟器，支持多用户环境下的进程管理、动态分区内存管理、多级反馈队列调度、二进制状态持久化、多实例共享和虚拟文件系统。

核心目标：
- 模拟操作系统的进程调度机制（MLFQ）
- 实现动态分区内存分配与回收
- 支持完整的系统状态二进制持久化
- 支持 Windows Named Pipe 多实例共享内核状态
- 提供全局可视化 overview 命令

## 2. 开发环境

| 项目 | 说明 |
|------|------|
| 操作系统 | Windows 11 Professional 64-bit |
| 开发工具 | Visual Studio 2022 Professional |
| 构建系统 | CMake 3.20+ |
| 语言标准 | C++20 |
| 编译器 | MSVC 19.x（Visual Studio 2022 自带） |
| 依赖 | 无第三方库，仅使用 C++ 标准库和 Windows API |

## 3. 系统总体架构

```text
┌─────────────────────────────────────────┐
│              ConsoleApp                  │
│     (MASTER: 本地+管道 / CLIENT: 管道)    │
├─────────────────────────────────────────┤
│                Kernel                    │
│  ┌──────────┐ ┌──────────┐ ┌─────────┐  │
│  │  Worker  │ │Scheduler │ │  Pipe   │  │
│  │  Thread  │ │  Thread  │ │ Server  │  │
│  └──────────┘ └──────────┘ └─────────┘  │
├─────────────────────────────────────────┤
│  UserMgr │ ProcessMgr │ MemoryMgr │ VFS │
├─────────────────────────────────────────┤
│       SnapshotStore (二进制持久化)        │
├─────────────────────────────────────────┤
│  InstanceGuard │ PipeServer │ PipeClient │
└─────────────────────────────────────────┘
```

## 4. 用户管理模块

功能：
- 用户注册：用户名 1-32 字符，密码 1-64 字符
- 用户登录：会话保持，允许多人使用不同账户
- 密码安全：salt + FNV-1a 散列，连续 3 次错误锁定
- 锁定持久化：锁定状态随快照保存和恢复

实现要点：
- UserManager 类封装所有用户操作
- 使用 std::unordered_map 存储用户名到 UserAccount 的映射
- 密码不保存明文

## 5. PCB 进程管理模块

功能：
- PCB 结构体包含：PID、PPID、名称、所有者、状态、优先级、CPU 时间等
- 8 种进程状态：NEW, READY, RUNNING, BLOCKED, SUSPENDED_READY, SUSPENDED_BLOCKED, TERMINATED, SWAPPED
- 10 个管理命令：create/kill/block/wakeup/suspend/resume/renice/show/list/ptree/readyq
- 用户级进程隔离：每个用户只能操作自己的进程

实现要点：
- 使用 std::unordered_map<PID, PCB> 管理进程表
- 递归删除子树，自动释放物理内存
- 维护 3 级 ready queue（对应 MLFQ）

## 6. MLFQ 多级反馈队列调度

功能：
- 三级队列：Q0（prio 0-3, 时间片 2）、Q1（prio 4-7, 时间片 4）、Q2（prio 8-15, 时间片 8）
- step 单步调度：逐 tick 推进，显示完整决策过程
- 自动调度：独立线程，每 500ms 执行一步
- 时间片耗尽 → 队列降级
- 进程完成 → 自动释放 PCB 和物理内存

实现要点：
- Scheduler 类独立于 ProcessManager
- 从高优先级队列开始扫描，跳过其他用户的进程
- 使用 stateMutex 保护，与命令线程互斥

## 7. 动态分区内存管理

功能：
- 总内存 1024KB，初始为单个 FREE 块
- 三种分配算法：First Fit / Best Fit / Worst Fit
- 内存释放后自动合并相邻空闲块
- compact 紧缩：消除外部碎片
- swap_out：模拟换出，释放物理内存

实现要点：
- std::vector\<MemoryBlock\> 按 start 排序
- compact 后同步更新 PCB 的 memStart
- 支持多用户内存隔离

## 8. 二进制持久化设计

功能：
- 文件格式：Magic(8B) + Version(4B) + HeaderSize(4B) + Flags(4B) + Data
- 版本 2：包含 VFS 数据
- 版本 1 兼容：读取时 VFS 保持空值
- save：先写临时文件，再 rename（原子操作）
- load：启动时自动检测并加载

快照内容：
- 用户列表（含锁定状态）
- PCB 表和就绪队列
- 内存分区表
- 调度器状态
- VFS 文件列表（v2）

## 9. Windows Named Pipe 多实例共享

功能：
- Named Mutex 选主：首个实例为 MASTER，后续为 CLIENT
- MASTER 启动 Named Pipe Server 线程
- CLIENT 通过管道转发所有命令
- 长度前缀协议：避免 overview 等长输出被截断
- Client exit 不影响 Master

实现要点：
- 管道名：`\\.\pipe\OS_SIM_PIPE_2026`
- 预创建下一个管道实例，消除连接间隙
- 使用 `stateMutex_` 保证所有命令串行执行

## 10. VFS 虚拟文件系统

功能：
- 每个用户独立文件集合
- 支持 touch/write/read/ls/rm 操作
- write_file 自动创建不存在的文件
- 通过二进制快照持久化

## 11. overview 系统总览可视化

功能：
- [1] 系统摘要：用户、调度器、内存统计
- [2] 进程树：ASCII 树形结构展示父子关系
- [3] 内存图：ASCII 条状图 + 明细表
- [4] MLFQ 队列：含异常检测和警告
- [5] Notes

## 12. 多线程同步机制

线程模型：
- Worker 线程：命令执行，通过阻塞队列与前台通信
- Scheduler 线程：自动调度，检查 atomic flag
- Pipe Server 线程：管道监听，将命令提交到 Worker
- 前台线程：读取用户输入，通过 future 等待结果

同步机制：
- `stateMutex_`：保护所有共享状态（PCB、内存、用户、VFS）
- `BlockingQueue`：condition_variable 实现的生产者-消费者
- `std::atomic<bool>`：schedulerRunning、shuttingDown
- `std::promise/future`：前台线程等待命令结果

## 13. 测试结果

单元测试覆盖：
- BlockingQueue FIFO / shutdown 唤醒
- 用户注册/登录/锁定/登出
- PCB 创建/状态转换/递归删除
- 动态分区分配/释放/紧缩/碎片
- MLFQ step/降级/完成
- save/load 完整恢复
- VFS 创建/读写/删除/隔离

验收测试：10 个测试脚本覆盖 P1-P9 全部功能

## 14. 问题与解决方案

| 问题 | 解决方案 |
|------|----------|
| Named Pipe 连接间隙导致 Client 超时 | 预创建下一个管道实例 |
| compact 后 PCB memStart 失效 | 遍历 pidNewStart 同步更新 |
| 三次错误锁定后持久化 | 锁定状态保存到快照 |
| overview 可能死锁 | 在锁内获取快照副本，释放锁后渲染 |
| PowerShell 不支持 `<` 重定向 | 使用 `cmd /c` 包装 |

## 15. 总结

本项目完整实现了操作系统核心模拟器的 9 个阶段功能，涵盖进程管理、内存管理、调度、持久化、多实例 IPC、VFS 和可视化等模块。程序运行稳定，通过单元测试和验收测试，满足课程设计全部要求。
