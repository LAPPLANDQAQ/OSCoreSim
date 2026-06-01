# Persistent OS Core Simulator

Persistent OS Core Simulator 是一个面向操作系统课程设计的 C++20 命令行内核模拟器。项目运行环境以 Windows 11 Professional、VS Code、CMake、MSVC / Visual Studio Build Tools 为主，不依赖 GUI 框架或数据库。

当前已完成阶段：

- P1：命令框架、`Kernel`、阻塞请求队列、前台输入线程与后台 worker 线程分离
- P2：用户注册、登录、登出、会话保持、密码散列存储、连续 3 次登录失败锁定账户
- P3：PCB 进程表、父子进程树、进程状态转换、用户级进程隔离、ready queues
- P4：动态分区内存管理、FF/BF/WF 分配算法、内存释放、紧凑、进程内存集成
- P5：三级 MLFQ 调度器、手动 `step`、自动调度线程、时间片消耗、队列降级、进程完成后释放内存
- P6：二进制快照持久化，保存和恢复用户、PCB、ready queues、内存分区、调度状态

后续阶段将继续实现 Windows 命名管道多实例共享访问、虚拟文件系统和总览命令。

## Project Structure

```text
OSCoreSim/
|- CMakeLists.txt
|- README.md
|- data/
|  `- os_state.bin       # P6 binary snapshot
|- docs/
|- src/
|  |- app/               # ConsoleApp 前台交互层
|  |- auth/              # UserAccount / UserManager
|  |- kernel/            # Kernel / CommandDispatcher / CommandTypes
|  |- process/           # PCB / ProcessManager / Scheduler
|  |- memory/            # 动态分区 MemoryManager
|  |- persistence/       # SnapshotStore 二进制快照
|  |- ipc/               # Windows IPC 占位模块
|  |- vfs/               # 虚拟文件系统占位模块
|  `- util/              # BlockingQueue、字符串工具
`- tests/
   |- demo_commands.txt
   `- unit_tests.cpp
```

## Architecture

命令执行链路：

```text
ConsoleApp 前台线程
  -> 读取用户输入
  -> Kernel::submitCommand()
  -> BlockingQueue<CommandRequest>
  -> Kernel 后台 worker 线程
  -> CommandDispatcher / Kernel-owned scheduler and persistence handlers
  -> UserManager / ProcessManager / MemoryManager / Scheduler / SnapshotStore
  -> CommandResponse
  -> ConsoleApp 打印结果
```

自动调度线程：

```text
Kernel scheduler thread
  -> 每 500ms 检查 schedulerRunning
  -> 在 Kernel stateMutex_ 保护下执行 Scheduler::step()
  -> 修改 PCB、ready queues、内存状态
  -> 打印调度日志
```

程序启动时，`Kernel::start()` 会检测 `data/os_state.bin`。如果文件存在且格式有效，会自动导入状态；为安全起见，不恢复登录会话，也不自动重启调度器。

## Build

如果 `cmake` 在 `PATH` 中：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

如果 VS Code 终端找不到 `cmake`，可以使用 Visual Studio 自带版本：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

生成的程序：

```text
build\Release\os_sim.exe
```

## Commands

基础命令：

| Command | Description |
|---|---|
| `help` | 显示可用命令 |
| `status` | 显示 worker、调度器、快照文件、当前用户、进程、ready queues、内存状态 |
| `clear` | 使用空行模拟清屏 |
| `exit` / `quit` | 退出程序并清理后台线程 |

持久化命令：

| Command | Description |
|---|---|
| `save` | 保存完整模拟器状态到 `data/os_state.bin` |
| `load` | 从 `data/os_state.bin` 加载完整模拟器状态 |

用户命令：

| Command | Description |
|---|---|
| `register <username> <password>` | 注册新用户 |
| `login <username> <password>` | 登录模拟器 |
| `logout` | 登出当前用户，并停止自动调度 |
| `whoami` | 显示当前登录用户，未登录时显示 `not logged in` |

进程命令：

| Command | Description |
|---|---|
| `create_pcb <name> <memKB> <priority> <totalTime> [ppid]` | 创建 PCB 并分配真实动态分区内存 |
| `kill_pcb <pid>` | 递归删除指定进程及其子进程，并释放内存 |
| `block_pcb <pid>` | READY / RUNNING 转 BLOCKED |
| `wakeup_pcb <pid>` | BLOCKED 转 READY |
| `show_pcb <pid>` | 查看单个 PCB 详情 |
| `list_pcb` | 列出当前用户的 PCB |
| `ptree` | 显示当前用户的进程树 |
| `suspend <pid>` | 挂起 READY / RUNNING / BLOCKED 进程 |
| `resume <pid>` | 恢复挂起进程 |
| `renice <pid> <newPriority>` | 修改优先级并更新队列等级 |
| `readyq` | 查看当前用户的 ready queues |

内存命令：

| Command | Description |
|---|---|
| `alloc <sizeKB>` | 手动分配当前用户的 KERNEL 内存块 |
| `free_mem <addr>` | 按起始地址释放当前用户的手动内存块 |
| `show_mem` | 显示内存分区表和 ASCII 内存图 |
| `compact` | 内存紧凑，并同步更新 PCB 的 `memStart` |
| `mem_stat` | 显示内存使用量和外部碎片率 |
| `set_alloc_algo <FF|BF|WF>` | 切换 First Fit / Best Fit / Worst Fit |
| `pgfault [pid]` | 模拟缺页中断处理流程 |
| `swap_out <pid>` | 模拟换出进程并释放物理内存 |

调度命令：

| Command | Description |
|---|---|
| `start_sched` | 启动当前登录用户的自动 MLFQ 调度 |
| `stop_sched` | 停止自动调度，保留进程运行状态 |
| `restart_sched` | 清理无效 ready queue 项并重新启动调度 |
| `step` | 执行一次调度决策并打印完整过程 |

## Binary Snapshot Format

物理文件固定为：

```text
data/os_state.bin
```

格式为纯二进制，不使用 JSON、TXT、CSV、XML 等文本格式。写入时先生成 `data/os_state.tmp`，成功关闭后再替换 `data/os_state.bin`。

文件头：

```cpp
magic      = "OSSM2026"  // 8 bytes
version    = 1           // uint32_t
headerSize = 20          // uint32_t
flags      = 0           // uint32_t
```

字段编码规则：

- 整数：原生 little-endian `uint32_t` / `int32_t`
- 布尔：`uint32_t`，0 表示 false，1 表示 true
- 字符串：`uint32_t length` + raw bytes
- 数组：`uint32_t count` + repeated items
- 枚举：`int32_t`

快照内容：

- 用户：用户名、密码 hash、salt、失败次数、账户状态
- 进程：`nextPid`、所有 PCB 字段、children 列表
- ready queues：Q0/Q1/Q2 的 PID 顺序
- 内存：总内存、分配算法、所有 MemoryBlock
- 调度：保存运行标志和 owner，但加载后统一停止调度

说明：该二进制格式带版本号，面向本课程 Windows 模拟器。跨平台端序转换不在当前阶段范围内。

## Persistence Rules

- `save` 不要求登录，会保存完整内核状态
- `load` 不要求登录，会停止自动调度并导入快照
- `load` 后主动清除登录会话，用户需要重新 `login`
- 程序启动时会自动尝试加载 `data/os_state.bin`
- 文件不存在时启动干净系统
- 文件损坏、魔数错误、版本不支持、读取截断时不会崩溃，也不会自动覆盖损坏文件
- 载入后会校验 PCB、ready queues 和内存块；ready queue 中的无效项会被清理

## Scheduler Rules

- Q0：最高优先级，时间片 2 tick
- Q1：中优先级，时间片 4 tick
- Q2：最低优先级，时间片 8 tick
- 初始优先级映射：`0-3 -> Q0`，`4-7 -> Q1`，`8-15 -> Q2`
- 每次调度从 Q0 到 Q2 扫描，选择第一个属于当前用户的 READY 进程
- BLOCKED、SUSPENDED、SWAPPED、TERMINATED 进程不会被调度
- 进程完成后会从 PCB 表删除，并通过 `MemoryManager` 释放对应物理内存

## Tests

构建后运行单元测试：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

如果 `ctest` 不在 `PATH` 中：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

当前测试覆盖：

- `BlockingQueue` FIFO 和 shutdown 唤醒
- 用户注册、重复注册、登录、登出、账户锁定
- PCB 创建、父子关系、用户隔离、ready queues
- 动态分区内存分配、释放、紧凑、碎片统计
- MLFQ `step`、自动调度启动/停止
- `save` / `load` 二进制快照
- 启动自动加载快照后清除登录会话
- 分配算法、进程和内存块恢复

## Current Limitations

以下功能尚未实现，属于后续阶段：

- Windows 命名管道 IPC
- 多实例共享同一个内核状态
- 完整虚拟文件系统
- `overview` 全局总览命令
- 报告生成

## Next Phase

建议下一阶段实现命名管道 IPC 和多实例共享访问：

- 主实例持有唯一内核状态
- 后续实例通过 Windows named pipe 转发命令
- 所有实例共享同一个 `data/os_state.bin`
- 保持当前命令行交互方式不变
