# Persistent OS Core Simulator

Command-line operating-system core simulator for a Windows 11, VS Code,
C++20, CMake, and MSVC course-design environment.

## Build

If `cmake` is on `PATH`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

If Visual Studio's bundled CMake is not on `PATH`, use:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

The executable is produced as:

```text
build\Release\os_sim.exe
```

## Run

```powershell
.\build\Release\os_sim.exe
```

Currently implemented commands:

- `help`
- `exit`

Other OS simulator modules are present as compile-ready placeholders for the
next implementation phases.
