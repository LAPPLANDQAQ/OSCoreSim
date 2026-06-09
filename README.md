# Persistent OS Core Simulator

Persistent OS Core Simulator 是一个面向操作系统课程设计的 C++20 命令行内核模拟器。项目运行环境为 Windows 11 Professional、Visual Studio 2022 Professional、MSVC、ISO C++20，不依赖 CMake、GUI 框架或数据库。

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
| P9 | 最小虚拟文件系统——touch_file/write_file/read_file/ls_file/rm_file 命令、用户隔离、二进制持久化接入 |

## Project Structure

```text
OSCoreSim/
│
├── OSCoreSim.sln                # Visual Studio 2022 解决方案
├── OSCoreSim/
│   ├── OSCoreSim.vcxproj        # 主程序项目（C++20, MSVC v143, x64）
│   └── OSCoreSim.vcxproj.filters # 源文件筛选器（模块分组）
├── OSCoreSimTests/
│   ├── OSCoreSimTests.vcxproj   # 单元测试项目
│   └── OSCoreSimTests.vcxproj.filters
├── README.md                   # 项目说明文档（本文件）
├── .gitignore                  # Git 忽略规则（build/、*.exe、.vs/ 等）
│
├── data/                       # 运行时数据目录
│   └── os_state.bin            # 二进制快照文件（v2 格式，save 命令生成，启动时自动加载）
│
├── docs/                       # 设计文档与课程报告资料
│
├── src/                        # 源代码根目录
│   │
│   ├── main.cpp                # 程序入口 —— 配置 UTF-8 控制台编码 → InstanceGuard 选主 → 启动 ConsoleApp
│   │
│   ├── app/                    # 【应用层】命令行交互前端
│   │   ├── ConsoleApp.h        #    ConsoleApp 类声明（masterLoop / clientLoop 双模式）
│   │   ├── ConsoleApp.cpp      #    Master 模式：启动内核 + PipeServer + 本地输入循环
│   │   │                       #    Client 模式：启动菜单 + NamedPipeClient 转发循环
│   │   ├── MenuConsole.h       #    中文数字菜单系统声明
│   │   └── MenuConsole.cpp     #    菜单命令树、快捷键映射、命令行备选入口
│   │
│   ├── auth/                   # 【认证模块】用户账户管理
│   │   ├── UserAccount.h       #    UserAccount 结构体 + AccountStatus 枚举（NORMAL / LOCKED）
│   │   ├── UserManager.h       #    UserManager 类声明
│   │   └── UserManager.cpp     #    注册/登录/登出、3 次错误锁定、FNV-1a 密码散列、导入导出
│   │
│   ├── kernel/                 # 【内核模块】命令调度与系统状态中枢
│   │   ├── CommandTypes.h      #    Command / CommandRequest / CommandResponse / CommandContext 类型定义
│   │   ├── CommandDispatcher.h #    CommandDispatcher 类声明（命令解析 + 分发）
│   │   ├── CommandDispatcher.cpp #  命令解析（parseUint32Strict）、help/status 文本生成、
│   │   │                       #    login/register/logout/whoami/进程/内存命令路由
│   │   ├── Kernel.h            #    Kernel 类声明（核心调度器，持有所有子系统）
│   │   └── Kernel.cpp          #    Kernel 实现：启动/停止 worker 线程 + 调度线程、
│   │                           #    自动加载快照、save/load/VFS/overview/调度命令统一处理、
│   │                           #    快照导出导入、状态校验、复位
│   │
│   ├── process/                # 【进程模块】PCB 管理与 MLFQ 调度
│   │   ├── PCB.h               #    PCB 结构体 + ProcessState 枚举（8 种状态）
│   │   ├── ProcessManager.h    #    ProcessManager 类声明
│   │   ├── ProcessManager.cpp  #    进程 CRUD、状态转换（block/wakeup/suspend/resume/renice）、
│   │   │                       #    父子进程树维护、就绪队列管理、导入导出
│   │   ├── Scheduler.h         #    Scheduler 类声明
│   │   └── Scheduler.cpp       #    MLFQ 三级调度 step()：Q0→Q1→Q2 扫描、时间片消耗、
│   │                           #    队列降级、进程完成后释放内存
│   │
│   ├── memory/                 # 【内存模块】动态分区内存管理
│   │   ├── MemoryBlock.h       #    MemoryBlock 结构体 + AllocAlgorithm / MemoryBlockType 枚举
│   │   ├── MemoryManager.h     #    MemoryManager 类声明
│   │   └── MemoryManager.cpp   #    FF/BF/WF 分配、释放、紧缩（compact）、空闲块合并、
│   │                           #    swap_out 释放、碎片统计、内存校验、导入导出
│   │
│   ├── persistence/            # 【持久化模块】二进制快照存储
│   │   ├── SnapshotStore.h     #    KernelSnapshot / SnapshotSummary 结构体 + SnapshotStore 类声明
│   │   └── SnapshotStore.cpp   #    二进制读写（BinaryWriter/Reader）、v2 头格式、用户/PCB/
│   │                           #    ready queues/内存/VFS 序列化、v1 兼容读取、原子写入
│   │
│   ├── ipc/                    # 【IPC 模块】Windows 多实例进程间通信
│   │   ├── InstanceGuard.h     #    InstanceGuard 类声明 + InstanceRole 枚举
│   │   ├── InstanceGuard.cpp   #    CreateMutexA 命名互斥量选主（MASTER / CLIENT）
│   │   ├── NamedPipeServer.h   #    NamedPipeServer 类声明
│   │   ├── NamedPipeServer.cpp #    管道服务器线程：创建管道 → 阻塞等待连接 →
│   │   │                       #    读取命令 → 调用 Kernel → 返回响应 → 预创建下一管道实例
│   │   ├── NamedPipeClient.h   #    NamedPipeClient 类声明
│   │   └── NamedPipeClient.cpp #    WaitNamedPipeA → CreateFileA → 发送命令 → 接收响应
│   │
│   ├── vfs/                    # 【VFS 模块】最小虚拟文件系统
│   │   ├── VirtualFileSystem.h #    VirtualFile 结构体 + VirtualFileSystem 类声明
│   │   └── VirtualFileSystem.cpp #  touch_file/write_file/read_file/ls_file/rm_file、用户隔离、文件名校验、导入导出
│   │
│   ├── view/                   # 【视图模块】全局可视化
│   │   ├── OverviewRenderer.h  #    OverviewRenderer 类声明
│   │   └── OverviewRenderer.cpp #   系统摘要 / 进程树（ASCII 树形）/ 内存图 / MLFQ 队列 /
│   │                           #    异常检测与警告 五段式渲染
│   │
│   └── util/                   # 【工具模块】通用组件
│       ├── BlockingQueue.h     #    线程安全阻塞队列（push/pop/shutdown/reset）
│       ├── Logger.h            #    简易日志输出（info/error）
│       ├── StringUtil.h        #    字符串工具（toLower / trim）
│       └── TablePrinter.h      #    表格打印辅助
│
└── tests/                      # 测试脚本与单元测试
    ├── unit_tests.cpp          # C++ 单元测试（BlockingQueue、用户、PCB、内存、调度、持久化、VFS）
    ├── demo_commands.txt       # 早期 P1-P2 演示命令集
    ├── full_demo_commands.txt  # P1-P9 完整现场演示脚本（40+ 条命令，可管道输入批量执行）
    ├── 01_user_test.txt        # 用户管理验收测试（注册/重复/锁定/登出）
    ├── 02_process_test.txt     # 进程管理验收测试（10 个进程命令）
    ├── 03_memory_test.txt      # 内存管理验收测试（alloc/free/algo/compact）
    ├── 04_scheduler_test.txt   # 调度器验收测试（step/start/stop/restart）
    ├── 05_persistence_test.txt # 持久化验收测试（save/exit/load 恢复验证）
    ├── 06_vfs_test.txt         # VFS 验收测试（touch_file/write_file/read_file/rm_file + 隔离）
    ├── 07_overview_test.txt    # overview 动态一致性验证
    └── 08_multi_instance_test.md # 双窗口 IPC 手动验收流程说明
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

使用 Visual Studio 2022 打开 `OSCoreSim.sln`，选择 `Release | x64` 配置，生成解决方案。

或使用 MSBuild 命令行：

```powershell
msbuild OSCoreSim.sln /p:Configuration=Release /p:Platform=x64
```

生成的可执行文件：

```text
x64\Release\OSCoreSim.exe         # 主程序
x64\Release\OSCoreSimTests.exe    # 单元测试
```

## Run

```powershell
# 交互模式（第一个实例自动成为 Master，默认进入中文数字菜单）
.\x64\Release\OSCoreSim.exe

# 脚本化演示（stdin 重定向时自动跳过菜单，直接进入原始命令模式）
cmd /c ".\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt"

# 多实例 IPC —— 打开两个终端
# 终端 A: .\x64\Release\OSCoreSim.exe    （Master）
# 终端 B: .\x64\Release\OSCoreSim.exe    （Client）
```

交互启动后会先显示中文数字菜单；在主菜单选择 `8. 进入原始命令模式` 后，会切换到原始命令提示符。脚本重定向输入时不会等待菜单编号，会直接执行原始命令脚本；在 CMD 中可直接运行 `.\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt`，在 PowerShell 中使用上面的 `cmd /c` 写法。

原始命令提示符：

```text
OS-SIM[MASTER]>    # 主控端
OS-SIM[CLIENT]>    # 客户端
```

### 工作目录与快照文件

程序使用相对路径 `data/os_state.bin` 读写二进制快照。**实际读取位置取决于程序启动时的当前工作目录：**

| 启动方式 | 快照路径 |
|----------|----------|
| 项目根目录运行 `.\x64\Release\OSCoreSim.exe` | `当前目录\data\os_state.bin` ✅ |
| 双击 `x64\Release\OSCoreSim.exe` | `x64\Release\data\os_state.bin` ❌ |
| VS 调试运行（工作目录设为项目根目录） | `项目根目录\data\os_state.bin` ✅ |

**建议**：始终从项目根目录启动程序。

**data/ 策略**：
- `data/` 是运行时状态目录
- 源码仓库只保留 `data/.gitkeep` 占位，不提交 `*.bin` 快照文件
- 发行版的 `data/os_state.bin` 由用户首次运行后生成，或手动放入演示用快照

### 发行版目录结构

```text
发行版/
├── OSCoreSim.exe              # 可执行程序
├── data/
│   └── os_state.bin        # 二进制快照（与 OSCoreSim.exe 同级）
├── tests/                  # 验收测试脚本
└── 运行说明.md
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
| `alloc <sizeKB>` | 手动分配 KERNEL 内存块，Tag 默认为 `manual` |
| `alloc <name> <sizeKB>` | 手动分配命名内存区，`name` 会显示在 `show_mem` 的 Tag 列 |
| `free_mem <addr>` | 按起始地址释放手动分配的内存 |
| `show_mem` | 显示内存分区表和 ASCII 内存图 |
| `compact` | 内存紧缩，同步更新 PCB 的 memStart |
| `mem_stat` | 显示内存使用量和外部碎片率 |
| `set_alloc_algo <FF\|BF\|WF>` | 切换分配算法 |
| `pgfault [pid]` | 模拟缺页中断处理流程 |
| `swap_out <pid>` | 模拟换出进程并释放物理内存 |

中文菜单中的“内存管理 → 手动分配内存”会连续收集“内存区名称 + 内存大小 KB”，底层仍执行现有 `alloc <name> <sizeKB>` 命令。每次分配后菜单会自动追加显示 `show_mem` 和 `list_pcb`，便于课堂演示观察内存分区与 PCB 状态；原始命令模式和脚本重定向模式不会自动追加这些菜单辅助输出。

### 调度命令

| 命令 | 说明 |
|------|------|
| `start_sched` | 启动当前用户的自动 MLFQ 调度 |
| `start` | `start_sched` 的兼容别名 |
| `stop_sched` | 停止自动调度 |
| `stop` | `stop_sched` 的兼容别名 |
| `restart_sched` | 清理无效 ready queue 条目并重启调度 |
| `restart` | `restart_sched` 的兼容别名 |
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
| `touch_file <name>` | 创建空虚拟文件（支持 UTF-8 中文文件名） |
| `write_file <name> <content>` | 写入内容，支持转义序列 `\n` `\r` `\t` `\\` `\"` |
| `read_file <name>` | 读取文件内容，多行内容带分隔线展示 |
| `ls_file` | 列出当前用户的虚拟文件 |
| `rm_file <name>` | 删除虚拟文件 |

文件名限制：
- 1-128 字节（UTF-8），不允许 ASCII 控制字符
- 禁止字符：`/` `\` `:` `*` `?` `"` `<` `>` `|`

原始命令模式写文件示例：
```text
write_file 课程笔记 第一行\n第二行\n第三行
read_file 课程笔记
```

菜单模式支持多行内容输入，以单独一行 `.` 结束。菜单内部自动将换行符转义为 `\n` 后发送给命令执行器。

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
- PID 会随 `save`/`load` 持久化；如果 `data/os_state.bin` 已存在，新进程会从快照中的 `nextPid` 继续分配，不一定从 1 开始
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
- 自动启动加载快照后不恢复登录会话，须重新 `login`
- 手动 `load` 会在快照中仍存在当前用户时保留会话；否则清除会话并提示重新登录
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
- 文件名：1-128 字节（UTF-8），支持中文等多字节字符
- 禁止字符：`/` `\` `:` `*` `?` `"` `<` `>` `|` 及 ASCII 控制字符
- 同一用户下文件名不可重复，不同用户可有同名文件
- `write_file` 文件不存在时自动创建
- `write_file` 内容支持转义序列：`\n` 换行、`\r` 回车、`\t` 制表、`\\` 反斜杠、`\"` 双引号
- 菜单模式支持多行内容输入，以单独一行 `.` 结束
- VFS 内容通过 save/load 持久化

## Tests

```powershell
# 单元测试
.\x64\Release\OSCoreSimTests.exe

# 验收测试（人工）
cmd /c ".\x64\Release\OSCoreSim.exe < tests\01_user_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\02_process_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\03_memory_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\04_scheduler_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt"
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
