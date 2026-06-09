# Visual Studio 2022 运行说明

## 构建环境

- Windows 10 / 11 64 位
- Visual Studio 2022 Professional（或 Community）
- C++20

## 构建步骤

### 使用 Visual Studio 2022

打开 `OSCoreSim.sln`，选择 `Release | x64` 配置，生成解决方案。

或使用 MSBuild 命令行：

```powershell
msbuild OSCoreSim.sln /p:Configuration=Release /p:Platform=x64
```

## 运行

### 交互模式

```powershell
.\x64\Release\OSCoreSim.exe
```

### 脚本演示

**PowerShell 不支持直接使用 `<` 输入重定向。必须使用 cmd /c：**

```cmd
cmd /c ".\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt"
```

### 双窗口 IPC

打开两个终端，分别运行：

```powershell
# 窗口 A（自动成为 MASTER）
.\x64\Release\OSCoreSim.exe

# 窗口 B（自动成为 CLIENT）
.\x64\Release\OSCoreSim.exe
```

## 工作目录说明

**重要：程序的当前工作目录影响 `data/os_state.bin` 的读写位置。**

程序使用相对路径 `data/os_state.bin` 保存和加载快照。实际读写路径取决于启动时的当前工作目录：

| 启动方式 | 当前工作目录 | 快照路径 |
|----------|-------------|----------|
| 项目根目录运行 `.\x64\Release\OSCoreSim.exe` | 项目根目录 | `项目根目录\data\os_state.bin` ✅ |
| 双击 `x64\Release\OSCoreSim.exe` | `x64\Release\` | `x64\Release\data\os_state.bin` ❌ |
| VS 2022 调试运行 | 取决于调试设置 | 建议设为项目根目录 |

**建议：**

- 总是从项目根目录启动：`.\x64\Release\OSCoreSim.exe`
- VS 调试时项目已设置工作目录为 `$(SolutionDir)`（项目根目录），无需手动调整
- 最终发行版中 `data/` 必须与 `OSCoreSim.exe` 放在同一级目录

## 测试

```powershell
# 单元测试
.\x64\Release\OSCoreSimTests.exe

# 验收测试（逐个执行）
cmd /c ".\x64\Release\OSCoreSim.exe < tests\01_user_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\02_process_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\03_memory_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\04_scheduler_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt"
```
