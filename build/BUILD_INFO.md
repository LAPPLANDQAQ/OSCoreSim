# 构建信息

本文件只记录可提交的非敏感构建信息。`build/` 下的 CMake 缓存、Visual Studio 工程文件、测试临时目录、可执行文件和调试符号仍属于本地生成物，不提交到仓库。

## 环境

- 平台：Windows
- 生成器：Visual Studio 17 2022
- 架构：x64
- 配置：Release
- 编译器：MSVC / Visual Studio 2022 Professional

## 构建命令

```powershell
$vsCMakeBin = "<VS2022_CMake_bin>"
& (Join-Path $vsCMakeBin "cmake.exe") -S . -B build -G "Visual Studio 17 2022" -A x64
& (Join-Path $vsCMakeBin "cmake.exe") --build build --config Release
& (Join-Path $vsCMakeBin "ctest.exe") --test-dir build -C Release --output-on-failure
```

## 本地生成产物

以下文件由构建命令生成，仅作为本地验证结果存在，不提交：

| 文件 | 说明 |
|------|------|
| `build\Release\os_sim.exe` | 主程序 |
| `build\Release\os_sim_tests.exe` | 单元测试程序 |
| `build\CMakeCache.txt` | 本机 CMake 缓存，可能包含本地路径 |
| `build\*.vcxproj` / `build\*.sln` | Visual Studio 生成工程 |
| `build\Testing\` | CTest 临时记录 |

## 最近验证结果

- CMake configure：通过
- Release build：通过
- CTest：`1/1 tests passed`
- `os_sim_tests.exe`：退出码 0
- `tests\full_demo_commands.txt` 重定向演示：退出码 0
- 原始命令兼容性：`alloc <sizeKB>` 保持可用，`alloc <name> <sizeKB>` 可显示命名 Tag

## 提交策略

- 提交本文件，作为构建与验证摘要。
- 不提交二进制、缓存、测试临时文件或含本机绝对路径的生成工程。
- 需要复现实验时，请按 README 中的 Build / Run 命令在本机重新生成 `build/` 内容。
