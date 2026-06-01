# Persistent OS Core Simulator

Persistent OS Core Simulator 是一个面向操作系统课程设计的命令行内核模拟器。
当前项目使用 C++20 和 CMake 构建，目标环境为 Windows 11 Professional、
VS Code、MSVC / Visual Studio Build Tools。

本项目按阶段实现。当前已完成：

- P1：核心命令框架、`Kernel`、后台工作线程、阻塞队列、前后台线程分离
- P2：用户注册、登录、登出、会话保持、密码错误 3 次锁定
- P3：PCB 进程表、父子进程树、进程状态转换、用户级进程隔离、基础就绪队列
- P4：动态分区内存管理、FF/BF/WF 分配算法、内存紧凑、进程内存接入

后续阶段会继续实现 MLFQ 调度、二进制持久化、
虚拟文件系统、Windows 命名管道多实例共享状态等功能。

## Project Structure

```text
OSCoreSim/
├─ CMakeLists.txt
├─ README.md
├─ data/
├─ docs/
├─ src/
│  ├─ app/              # ConsoleApp 前台交互层
│  ├─ auth/             # UserAccount / UserManager
│  ├─ kernel/           # Kernel / CommandDispatcher / CommandTypes
│  ├─ process/          # PCB、进程管理（已实现）和调度占位模块
│  ├─ memory/           # 动态分区内存管理（已实现）
│  ├─ persistence/      # 二进制快照占位模块
│  ├─ ipc/              # Windows IPC 占位模块
│  ├─ vfs/              # 虚拟文件系统占位模块
│  └─ util/             # BlockingQueue、字符串和日志工具
└─ tests/
   ├─ demo_commands.txt
   └─ unit_tests.cpp
```

## Architecture

当前命令执行链路如下：

```text
ConsoleApp 前台线程
  -> 读取用户输入
  -> Kernel::submitCommand()
  -> BlockingQueue<CommandRequest>
  -> Kernel 后台 worker 线程
  -> CommandDispatcher
  -> UserManager / ProcessManager / MemoryManager
  -> CommandResponse
  -> ConsoleApp 打印结果
```

前台线程只负责读取输入和输出响应。命令解析、用户管理、进程操作、内存分配和调度
统一通过 `Kernel` 和 `CommandDispatcher` 执行，`ProcessManager` 和 `MemoryManager`
作为独立模块由 `Kernel` 持有并通过 `dispatch()` 注入。

## Build

如果 `cmake` 已在 `PATH` 中：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

如果 VS Code 终端找不到 `cmake`，可以使用 Visual Studio 自带的 CMake：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

生成的可执行文件：

```text
build\Release\os_sim.exe
```

## Run

```powershell
.\build\Release\os_sim.exe
```

启动后提示符为：

```text
OS-SIM>
```

## Commands

基础命令：

| Command | Description |
|---|---|
| `help` | 显示可用命令 |
| `status` | 显示 worker 状态、当前用户、进程数、就绪队列、内存使用和分配算法 |
| `save` | 二进制保存占位命令 |
| `load` | 二进制加载占位命令 |
| `clear` | 使用空行模拟清屏 |
| `exit` | 退出程序并关闭后台 worker 线程 |
| `quit` | 同 `exit` |

用户命令：

| Command | Description |
|---|---|
| `register <username> <password>` | 注册新用户 |
| `login <username> <password>` | 登录模拟器 |
| `logout` | 登出当前用户 |
| `whoami` | 显示当前登录用户，未登录时显示 `not logged in` |

进程命令：

| Command | Description |
|---|---|
| `create_pcb <name> <memKB> <priority> <totalTime> [ppid]` | 创建 PCB，进入 READY 状态 |
| `kill_pcb <pid>` | 递归删除指定进程及其子进程 |
| `block_pcb <pid>` | READY / RUNNING 转 BLOCKED |
| `wakeup_pcb <pid>` | BLOCKED 转 READY |
| `show_pcb <pid>` | 查看单个 PCB 详情 |
| `list_pcb` | 列出当前用户的 PCB |
| `ptree` | 显示当前用户的进程树 |
| `suspend <pid>` | 挂起 READY / RUNNING / BLOCKED 进程 |
| `resume <pid>` | 恢复挂起进程 |
| `renice <pid> <newPriority>` | 修改优先级并更新队列等级 |
| `readyq` | 查看当前用户的基础就绪队列 |

内存命令：

| Command | Description |
|---|---|
| `alloc <sizeKB>` | 手动分配当前用户的 KERNEL 内存块 |
| `free_mem <addr>` | 按起始地址释放当前用户的手动分配内存 |
| `show_mem` | 显示内存分区表和 ASCII 内存图 |
| `compact` | 内存紧凑，将已分配块移动到低地址 |
| `mem_stat` | 显示内存使用率和外部碎片率 |
| `set_alloc_algo <FF|BF|WF>` | 切换 First Fit / Best Fit / Worst Fit |
| `pgfault [pid]` | 模拟缺页中断处理流程 |
| `swap_out <pid>` | 模拟换出进程并释放物理内存 |

## User Rules

用户管理当前已实现以下规则：

- 用户名长度：1 到 32 个字符
- 密码长度：1 到 64 个字符
- 用户名只能包含英文字母、数字、下划线 `_` 或连字符 `-`
- 禁止重复注册同名用户
- 密码不以明文保存，当前使用 `salt + password` 的课程级散列方案
- 密码连续错误 3 次后账户锁定
- 账户锁定后，即使输入正确密码也不能登录
- 当前会话保持到 `logout` 或程序退出

说明：当前密码散列方案只用于课程设计模拟，不是生产级密码学方案。

## Process Rules

PCB 管理当前已实现以下规则：

- 所有进程命令必须先登录
- 每个 PCB 记录 `owner`，用户只能查看和操作自己的进程
- PID 全局递增，当前不会因为删除进程而复用
- `priority` 范围为 0 到 15
- 优先级映射到基础队列：
  - `0-3 -> Q0`
  - `4-7 -> Q1`
  - `8-15 -> Q2`
- 时间片初值：
  - `Q0 = 2`
  - `Q1 = 4`
  - `Q2 = 8`
- `create_pcb` 会调用 `MemoryManager` 分配真实动态分区，失败时不会创建 PCB
- `kill_pcb` 会递归删除子进程，并从 ready queue 和父进程 children 列表中清理引用
- `kill_pcb` 会释放被删除进程及子进程占用的物理内存
- 当前只维护基础 ready queues，不执行真实 MLFQ 自动调度

## Memory Rules

动态分区内存管理当前已实现以下规则：

- 总内存默认 `1024KB`
- 初始布局为一个 `FREE` 块
- 支持 `FF`、`BF`、`WF` 三种分配算法
- 手动 `alloc` 生成 `KERNEL` 块，`tag=manual`
- `create_pcb` 生成 `PROCESS` 块，`pid` 和 PCB 对应
- 当前用户只能释放自己拥有的手动内存块
- 其他用户的已分配块可在 `show_mem` 中只读显示为 `OTHER_USER`
- `compact` 会更新进程 PCB 的 `memStart`
- `swap_out` 会释放进程物理内存，将 PCB 标记为 `SWAPPED`
- `pgfault` 只打印模拟处理流程，不实现真实分页

## Demo Commands

可以参考 [tests/demo_commands.txt](tests/demo_commands.txt)：

```text
help
status
whoami
register alice 123456
register alice 999999
login alice wrong1
login alice wrong2
login alice wrong3
login alice 123456
register bob abc
login bob wrong
login bob abc
whoami
status
logout
whoami
save
load
unknown_command
clear
create_pcb test 64 0 10
login bob abc
create_pcb init 64 0 20
create_pcb shell 128 5 12 1
create_pcb worker 32 8 8 2
create_pcb logger 32 10 8 1
list_pcb
show_pcb 2
ptree
readyq
block_pcb 2
list_pcb
readyq
wakeup_pcb 2
suspend 3
resume 3
renice 4 2
ptree
kill_pcb 1
list_pcb
ptree
readyq
show_mem
mem_stat
alloc 100
alloc 80
show_mem
free_mem 0
set_alloc_algo BF
alloc 50
set_alloc_algo WF
alloc 30
show_mem
compact
show_mem
create_pcb p1 64 0 10
create_pcb p2 128 5 20
show_mem
swap_out 6
show_pcb 6
pgfault 5
logout
register charlie abc
login charlie abc
show_mem
free_mem 0
alloc 40
show_mem
exit
```

也可以直接在 PowerShell 中进行一次脚本化演示：

```powershell
@(
  'help',
  'status',
  'whoami',
  'register alice 123456',
  'register alice 999999',
  'login alice wrong1',
  'login alice wrong2',
  'login alice wrong3',
  'login alice 123456',
  'register bob abc',
  'login bob wrong',
  'login bob abc',
  'whoami',
  'status',
  'logout',
  'whoami',
  'save',
  'load',
  'unknown_command',
  'clear',
  'create_pcb test 64 0 10',
  'login bob abc',
  'create_pcb init 64 0 20',
  'create_pcb shell 128 5 12 1',
  'create_pcb worker 32 8 8 2',
  'create_pcb logger 32 10 8 1',
  'list_pcb',
  'show_pcb 2',
  'ptree',
  'readyq',
  'block_pcb 2',
  'list_pcb',
  'readyq',
  'wakeup_pcb 2',
  'suspend 3',
  'resume 3',
  'renice 4 2',
  'ptree',
  'kill_pcb 1',
  'list_pcb',
  'ptree',
  'readyq',
  'show_mem',
  'mem_stat',
  'alloc 100',
  'alloc 80',
  'show_mem',
  'free_mem 0',
  'set_alloc_algo BF',
  'alloc 50',
  'set_alloc_algo WF',
  'alloc 30',
  'show_mem',
  'compact',
  'show_mem',
  'create_pcb p1 64 0 10',
  'create_pcb p2 128 5 20',
  'show_mem',
  'swap_out 6',
  'show_pcb 6',
  'pgfault 5',
  'logout',
  'register charlie abc',
  'login charlie abc',
  'show_mem',
  'free_mem 0',
  'alloc 40',
  'show_mem',
  'exit'
) | .\build\Release\os_sim.exe
```

## Tests

构建后运行单元测试：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

如果 `ctest` 不在 `PATH` 中，可使用 Visual Studio 自带版本：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

当前测试覆盖：

- `BlockingQueue` FIFO 和 shutdown 唤醒
- 命令解析
- Kernel 后台 worker 命令执行链路
- 用户注册、重复注册、登录、登出、锁定
- 密码不以明文保存在 `UserAccount`
- PCB 创建、父子关系、用户隔离、ready queue
- `block_pcb`、`wakeup_pcb`、`suspend`、`resume`、`renice`
- `kill_pcb` 递归删除子树
- 动态分区内存分配、释放、紧凑、碎片统计
- `create_pcb` / `kill_pcb` 与进程物理内存分配释放集成
- `swap_out`、`pgfault` 模拟命令

## Current Limitations

以下功能尚未实现，属于后续阶段：

- MLFQ 多级反馈队列调度
- 虚拟文件系统
- 二进制 `data/os_state.bin` 持久化
- Windows 命名管道 IPC
- 多实例共享同一个内核状态
- `overview` 全局可视化命令

## Next Phase

建议下一阶段实现 MLFQ 调度器：

- `start_sched`
- `stop_sched`
- `restart_sched`
- `step`

P5 应基于当前 PCB ready queues 实现真实 CPU 时间消耗、时间片递减、
队列降级和进程结束后的内存释放。
