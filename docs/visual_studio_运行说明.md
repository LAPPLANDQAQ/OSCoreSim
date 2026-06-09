# Visual Studio 2022 运行说明

## 构建环境

- Windows 10 / 11 64 位
- Visual Studio 2022 Professional（或 Community）
- CMake（VS 自带或独立安装）
- C++20

## 构建步骤

### 方式一：使用 VS 自带 CMake（推荐）

```powershell
$vsCMakeBin = "<VS2022_CMake_bin>"
& (Join-Path $vsCMakeBin "cmake.exe") -S . -B build -G "Visual Studio 17 2022" -A x64
& (Join-Path $vsCMakeBin "cmake.exe") --build build --config Release
```

### 方式二：使用系统 PATH 中的 cmake

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 运行

### 交互模式

```powershell
.\build\Release\os_sim.exe
```

### 脚本演示

**PowerShell 不支持直接使用 `<` 输入重定向。必须使用 cmd /c：**

```cmd
cmd /c ".\build\Release\os_sim.exe < tests\full_demo_commands.txt"
```

### 双窗口 IPC

打开两个终端，分别运行：

```powershell
# 窗口 A（自动成为 MASTER）
.\build\Release\os_sim.exe

# 窗口 B（自动成为 CLIENT）
.\build\Release\os_sim.exe
```

## 工作目录说明

**重要：程序的当前工作目录影响 `data/os_state.bin` 的读写位置。**

程序使用相对路径 `data/os_state.bin` 保存和加载快照。实际读写路径取决于启动时的当前工作目录：

| 启动方式 | 当前工作目录 | 快照路径 |
|----------|-------------|----------|
| 项目根目录运行 `.\build\Release\os_sim.exe` | 项目根目录 | `项目根目录\data\os_state.bin` ✅ |
| 双击 `build\Release\os_sim.exe` | `build\Release\` | `build\Release\data\os_state.bin` ❌ |
| VS 2022 调试运行 | 取决于调试设置 | 建议设为项目根目录 |

**建议：**

- 总是从项目根目录启动：`.\build\Release\os_sim.exe`
- VS 调试时在项目属性中设置"调试 → 工作目录"为 `$(ProjectDir)\..\..`
- 最终发行版中 `data/` 必须与 `os_sim.exe` 放在同一级目录

## 测试

```powershell
# 单元测试
$vsCMakeBin = "<VS2022_CMake_bin>"
& (Join-Path $vsCMakeBin "ctest.exe") --test-dir build -C Release --output-on-failure

# 验收测试（逐个执行）
cmd /c ".\build\Release\os_sim.exe < tests\01_user_test.txt"
cmd /c ".\build\Release\os_sim.exe < tests\02_process_test.txt"
cmd /c ".\build\Release\os_sim.exe < tests\03_memory_test.txt"
cmd /c ".\build\Release\os_sim.exe < tests\04_scheduler_test.txt"
cmd /c ".\build\Release\os_sim.exe < tests\full_demo_commands.txt"
```
