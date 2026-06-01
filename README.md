# Persistent OS Core Simulator

Persistent OS Core Simulator 是一个面向操作系统课程设计的 C++20 命令行内核模拟器。项目运行环境为 Windows 11 Professional、VS Code、CMake、MSVC / Visual Studio Build Tools，不依赖 GUI 框架或数据库。

## 已完成阶段

| 阶段 | 内容 |
|------|------|
| P1 | 命令框架、`Kernel`、阻塞请求队列、前台输入线程与后台 worker 线程分离 |
| P2 | 用户注册/登录/登出/会话保持、密码散列存储、连续 3 次登录失败锁定账户 |
| P3 | PCB 进程表、父子进程树、进程状态转换、用户级进程隔离、ready queues（10 命令） |
| P4 | 动态分区内存管理、FF/BF/WF 分配算法、内存释放/紧凑、进程内存集成（8 命令） |
| P5 | 三级 MLFQ 调度器、手动 `step`、自动调度线程、时间片消耗、队列降级、进程完成后释放内存 |
| P6 | 二进制快照持久化（v2 格式），保存/恢复用户、PCB、ready queues、内存分区、调度状态、VFS |
| P7 | `overview` 全局可视化——进程树、内存图、MLFQ 队列、系统摘要一体化展示 |
| P8 | Windows Named Pipe 多实例共享——Master/Client 选主、管道 IPC、双窗口共享内核状态 |
| P9 | 最小虚拟文件系统——touch/write/read/ls/rm 命令、用户隔离、二进制持久化接入 |

## Project Structure

```text
OSCoreSim/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── data/                    # 二进制快照文件目录
├── docs/                    # 文档目录
├── src/
│   ├── main.cpp             # 入口（InstanceGuard 选主 → ConsoleApp）
│   ├── app/                 # ConsoleApp 前台交互（Master/Client 双模式）
│   ├── auth/                # UserAccount / UserManager
│   ├── kernel/              # Kernel / CommandDispatcher / CommandTypes
│   ├── process/             # PCB / ProcessManager / Scheduler（MLFQ）
│   ├── memory/              # MemoryManager 动态分区（FF/BF/WF）
│   ├── persistence/         # SnapshotStore 二进制快照（v2）
│   ├── ipc/                 # InstanceGuard / NamedPipeServer / NamedPipeClient
│   ├── vfs/                 # VirtualFileSystem 最小虚拟文件系统
│   ├── view/                # OverviewRenderer 全局可视化渲染器
│   └── util/                # BlockingQueue / Logger / StringUtil / TablePrinter
└── tests/
    ├── unit_tests.cpp       # 单元测试（C++ 断言）
    ├── demo_commands.txt    # 早期演示命令
    ├── full_demo_commands.txt  # P1-P9 完整演示脚本
    ├── 01_user_test.txt     # 用户管理验收测试
    ├── 02_process_test.txt  # 进程管理验收测试
    ├── 03_memory_test.txt   # 内存管理验收测试
    ├── 04_scheduler_test.txt # 调度器验收测试
    ├── 05_persistence_test.txt # 持久化验收测试
    ├── 06_vfs_test.txt      # VFS 验收测试
    ├── 07_overview_test.txt # overview 验收测试
    └── 08_multi_instance_test.md # 多实例 IPC 验收流程
```

## Architecture

**命令执行链路：**

```text
ConsoleApp 前台线程
  → 读取用户输入
  → Kernel::submitCommand()
  → BlockingQueue<CommandRequest>
  → Kernel 后台 worker 线程
  → CommandDispatcher / Kernel-owned handlers
  → UserManager / ProcessManager / MemoryManager / Scheduler / SnapshotStore / VFS
  → CommandResponse
  → ConsoleApp 打印结果
```

**Master 模式（第一个实例）：**

```text
main() → InstanceGuard → ConsoleApp::masterLoop()
  → Kernel::start()（worker 线程 + 调度线程 + 自动加载快照）
  → NamedPipeServer::start()（管道服务器线程）
  → 本地命令输入循环 → submitCommand()
```

**Client 模式（后续实例）：**

```text
main() → InstanceGuard → ConsoleApp::clientLoop()
  → 本地命令输入循环 → NamedPipeClient::sendCommand()
  → \\.\pipe\OS_SIM_PIPE_2026 → Master Kernel → 返回响应
```

**自动调度线程：**

```text
Kernel scheduler thread
  → 每 500ms 检查 schedulerRunning
  → 在 stateMutex_ 保护下执行 Scheduler::step()
  → 修改 PCB、ready queues、内存状态
  → 打印调度日志
```

## Build

```powershell
# 使用系统 PATH 中的 cmake
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 或使用 Visual Studio 自带的 CMake
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

生成的可执行文件：

```text
build\Release\os_sim.exe         # 主程序
build\Release\os_sim_tests.exe   # 单元测试
```

## Run

```powershell
# 交互模式（第一个实例自动成为 Master）
.\build\Release\os_sim.exe

# 脚本化演示
.\build\Release\os_sim.exe < tests\full_demo_commands.txt

# 多实例 IPC —— 打开两个终端
# 终端 A: .\build\Release\os_sim.exe    （Master）
# 终端 B: .\build\Release\os_sim.exe    （Client）
```

启动后提示符：

```text
OS-SIM[MASTER]>    # 主控端
OS-SIM[CLIENT]>    # 客户端
```

## Commands

### 基础命令

| 命令 | 说明 |
|------|------|
| `help` | 显示可用命令列表 |
| `status` | 显示内核状态（worker、调度器、用户、进程、内存） |
| `clear` | 使用空行模拟清屏 |
| `exit` / `quit` | 安全退出（Master 会先停 PipeServer 再停 Kernel） |

### 用户命令

| 命令 | 说明 |
|------|------|
| `register <username> <password>` | 注册新用户 |
| `login <username> <password>` | 登录模拟器 |
| `logout` | 登出当前用户，同时停止自动调度 |
| `whoami` | 显示当前登录用户 |

### 进程命令

| 命令 | 说明 |
|------|------|
| `create_pcb <name> <memKB> <priority> <totalTime> [ppid]` | 创建 PCB 并分配动态分区内存 |
| `kill_pcb <pid>` | 递归删除进程子树，释放内存 |
| `block_pcb <pid>` | 阻塞进程（READY/RUNNING → BLOCKED） |
| `wakeup_pcb <pid>` | 唤醒进程（BLOCKED → READY） |
| `show_pcb <pid>` | 查看单个 PCB 详情 |
| `list_pcb` | 列出当前用户所有 PCB |
| `ptree` | 显示当前用户进程树 |
| `suspend <pid>` | 挂起进程 |
| `resume <pid>` | 恢复挂起进程 |
| `renice <pid> <newPriority>` | 修改优先级并更新队列等级 |
| `readyq` | 查看当前用户就绪队列 |

### 内存命令

| 命令 | 说明 |
|------|------|
| `alloc <sizeKB>` | 手动分配 KERNEL 内存块 |
| `free_mem <addr>` | 按起始地址释放手动分配的内存 |
| `show_mem` | 显示内存分区表和 ASCII 内存图 |
| `compact` | 内存紧缩，同步更新 PCB 的 memStart |
| `mem_stat` | 显示内存使用量和外部碎片率 |
| `set_alloc_algo <FF\|BF\|WF>` | 切换分配算法 |
| `pgfault [pid]` | 模拟缺页中断处理流程 |
| `swap_out <pid>` | 模拟换出进程并释放物理内存 |

### 调度命令

| 命令 | 说明 |
|------|------|
| `start_sched` | 启动当前用户的自动 MLFQ 调度 |
| `stop_sched` | 停止自动调度 |
| `restart_sched` | 清理无效 ready queue 条目并重启调度 |
| `step` | 执行一次调度决策并打印完整过程 |

### 持久化命令

| 命令 | 说明 |
|------|------|
| `save` | 保存完整状态到 `data/os_state.bin` |
| `load` | 从 `data/os_state.bin` 加载状态 |

### 可视化命令

| 命令 | 说明 |
|------|------|
| `overview` | 全局快照——进程树、内存图、MLFQ 队列、系统摘要 |

### 虚拟文件命令

| 命令 | 说明 |
|------|------|
| `touch_file <name>` | 创建空虚拟文件 |
| `write_file <name> <content>` | 写入内容（文件不存在则自动创建） |
| `read_file <name>` | 读取文件内容 |
| `ls_file` | 列出当前用户的虚拟文件 |
| `rm_file <name>` | 删除虚拟文件 |

## User Rules

- 用户名：1-32 字符，仅字母/数字/下划线/连字符
- 密码：1-64 字符
- 禁止重复注册同名用户
- 密码使用 salt + FNV-1a 风格散列（课程设计级别）
- 连续 3 次登录失败 → 账户锁定
- 锁定后即使正确密码也不能登录
- 会话保持到 `logout` 或程序退出

## Process Rules

- 所有进程命令必须先登录
- PCB 记录 `owner`，用户只能操作自己的进程
- PID 全局递增，不回收复用
- 优先级 0-15，映射 Q0(0-3) / Q1(4-7) / Q2(8-15)
- 时间片：Q0=2 / Q1=4 / Q2=8
- `create_pcb` 通过 MemoryManager 分配真实动态分区，失败回滚不创建 PCB
- `kill_pcb` 递归删除子进程，释放物理内存，清理 ready queue 和 children 引用

## Memory Rules

- 总内存默认 1024KB
- 初始布局为单个 FREE 块
- 支持 FF / BF / WF 三种分配算法
- `alloc` 生成 KERNEL 块（tag=manual）
- `create_pcb` 生成 PROCESS 块
- 当前用户只能释放自己的手动内存块
- `compact` 后自动更新 PCB 的 memStart
- `swap_out` 释放物理内存，标记 PCB 为 SWAPPED

## Scheduler Rules

- Q0(prio 0-3, quantum=2) > Q1(prio 4-7, quantum=4) > Q2(prio 8-15, quantum=8)
- 每步从 Q0 开始扫描，选择第一个属于当前用户的 READY 进程
- BLOCKED/SUSPENDED/SWAPPED/TERMINATED 进程不可调度
- 时间片耗尽 → 降级到下一级队列
- 进程完成 → 删除 PCB + 释放物理内存

## Binary Snapshot Format

文件：`data/os_state.bin`（纯二进制，先写 `.tmp` 再 rename）

**文件头：**

```cpp
magic      = "OSSM2026"  // 8 bytes
version    = 2           // uint32_t（v1→v2 增加 VFS 字段）
headerSize = 20          // uint32_t
flags      = 0           // uint32_t
```

**字段编码：**

- 整数：原生 little-endian `uint32_t` / `int32_t` / `uint64_t`
- 布尔：`uint32_t`（0/1）
- 字符串：`uint32_t length` + raw bytes
- 数组：`uint32_t count` + repeated items
- 枚举：`int32_t`

**快照内容（按存储顺序）：**

1. 用户列表（username / passwordHash / salt / failedAttempts / status）
2. nextPid + PCB 列表（含 children 数组）
3. ready queues（Q0/Q1/Q2 的 PID 顺序）
4. 内存（totalMemoryKB + allocAlgorithm + MemoryBlock 列表）
5. 调度器（schedulerRunning + schedulerOwner）
6. VFS（nextFileId + VirtualFile 列表）—— **v2 新增**

**版本兼容：** v1 快照可被读取，VFS 字段保持空值。

## Persistence Rules

- `save` / `load` 不要求登录
- `load` 后清除登录会话，须重新 `login`
- 启动时自动检测并加载 `data/os_state.bin`
- 快照不存在 → 启动干净系统
- 快照损坏/版本不支持 → 提示错误，不覆盖损坏文件
- 载入后校验 PCB、ready queues、内存块有效性

## Multi-Instance IPC

- 第一个实例通过 Windows Named Mutex 成为 Master
- Master 持有唯一内核状态，启动 Named Pipe Server
- 后续实例检测到互斥量已存在 → 成为 Client
- Client 不创建内核，所有命令通过管道转发给 Master
- 管道：`\\.\pipe\OS_SIM_PIPE_2026`（长度前缀协议）
- Client `exit` 只关闭自身，不影响 Master
- Master `exit` 先停 PipeServer 再停 Kernel

## VFS Rules

- 每个用户拥有独立虚拟文件集合
- 文件名：1-64 字符，字母/数字/下划线/横线/点号
- 同一用户下文件名不可重复，不同用户可有同名文件
- `write_file` 文件不存在时自动创建
- VFS 内容通过 save/load 持久化

## Tests

```powershell
# 单元测试
ctest --test-dir build -C Release --output-on-failure

# 验收测试（人工）
.\build\Release\os_sim.exe < tests\01_user_test.txt
.\build\Release\os_sim.exe < tests\02_process_test.txt
.\build\Release\os_sim.exe < tests\03_memory_test.txt
.\build\Release\os_sim.exe < tests\04_scheduler_test.txt
.\build\Release\os_sim.exe < tests\full_demo_commands.txt
```

**单元测试覆盖：**

- BlockingQueue FIFO / shutdown 唤醒
- 用户注册/重复/登录/锁定/登出
- PCB 创建/父子关系/用户隔离/ready queues
- block/wakeup/suspend/resume/renice/kill 递归子树
- 动态分区分配/释放/紧凑/碎片统计
- MLFQ step / 自动调度启停
- save/load 二进制快照 + 启动自动加载
- 分配算法切换 + 进程内存块恢复

## Current Limitations

- 多实例共享同一用户会话（单 session 设计）
- VFS 无目录层级（扁平文件空间）
- 无文件打开表、inode、复杂权限位
- Windows 专有实现（Named Pipe / Named Mutex）
