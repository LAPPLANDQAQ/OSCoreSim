# Persistent OS Core Simulator

Persistent OS Core Simulator 是一个面向操作系统课程设计的命令行内核模拟器。
当前项目使用 C++20 和 CMake 构建，目标环境为 Windows 11 Professional、
VS Code、MSVC / Visual Studio Build Tools。

本项目按阶段实现。当前已完成：

- P1：核心命令框架、`Kernel`、后台工作线程、阻塞队列、前后台线程分离
- P2：用户注册、登录、登出、会话保持、密码错误 3 次锁定

后续阶段会继续实现 PCB、进程树、MLFQ 调度、动态分区内存、二进制持久化、
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
│  ├─ process/          # PCB、进程管理和调度占位模块
│  ├─ memory/           # 动态分区内存占位模块
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
  -> UserManager / placeholder command
  -> CommandResponse
  -> ConsoleApp 打印结果
```

前台线程只负责读取输入和输出响应。命令解析、用户状态修改和后续 OS 资源操作
统一通过 `Kernel` 和 `CommandDispatcher` 执行，便于后续接入进程、内存、调度
和多实例 IPC。

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
| `status` | 显示内核 worker 状态、调度器占位状态和当前用户 |
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

## Current Limitations

以下功能尚未实现，属于后续阶段：

- PCB 创建、终止、阻塞、唤醒、挂起、恢复
- 进程树和进程所有者隔离
- MLFQ 多级反馈队列调度
- 动态分区内存管理
- 虚拟文件系统
- 二进制 `data/os_state.bin` 持久化
- Windows 命名管道 IPC
- 多实例共享同一个内核状态
- `overview` 全局可视化命令

## Next Phase

建议下一阶段实现 PCB / ProcessManager：

- `create_pcb <name> <memKB> <priority> <totalTime> [ppid]`
- `kill_pcb <pid>`
- `show_pcb <pid>`
- `list_pcb`
- `ptree`

进程记录应绑定当前登录用户，为后续用户隔离、内存分配和调度打基础。
