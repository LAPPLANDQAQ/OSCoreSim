# Phase 4 Full Regression Report

Date: 2026-06-09
Branch: `No_Cmake_version`
Workspace: `C:\code\OSCoreSim`

## Scope

This phase rebuilt and regression-tested the Visual Studio project after the Phase 3 minimal runtime fixes.

No source code was changed during Phase 4. The only file added in this phase is this report.

## Cleanup

| Item | Result |
|---|---|
| `x64/` | Removed before rebuild |
| `.vs/` | Present, not removed; clearing it was not needed |
| `data/os_state.bin` | Present before test run; backed up before script execution and restored afterward |

## Build Results

Initial sandboxed MSBuild access failed because MSBuild could not read `C:\Users\wth\AppData\Local\Microsoft SDKs`. The same commands succeeded outside the sandbox.

| Command | Result |
|---|---|
| `msbuild OSCoreSim.sln /p:Configuration=Debug /p:Platform=x64` | PASS, 0 warnings, 0 errors |
| `msbuild OSCoreSim.sln /p:Configuration=Release /p:Platform=x64` | PASS, 0 warnings, 0 errors |

## Unit Test Result

| Command | Result |
|---|---|
| `.\x64\Release\OSCoreSimTests.exe` | PASS, exit code 0 |

## Diagnostic Script Results

All diagnostic scripts exited with code 0.

| Script | Result | Notes |
|---|---:|---|
| `tests\diagnostics\00_clean_boot_test.txt` | PASS | Run from a temporary clean working directory with no snapshot; first created process was `PID=1` |
| `tests\diagnostics\01_user_session_test.txt` | PASS | Covered registration, login/logout, failed login lockout |
| `tests\diagnostics\02_process_test.txt` | PASS | Process command routes worked; PID values followed persisted `nextPid` |
| `tests\diagnostics\03_memory_test.txt` | PASS | Memory commands worked; intentional invalid free/page/swap probes reported expected failures |
| `tests\diagnostics\04_scheduler_test.txt` | PASS | `step`, `start_sched`, `stop_sched`, `restart_sched`, plus aliases `start`, `stop`, `restart` were recognized |
| `tests\diagnostics\05_persistence_test.txt` | PASS | Manual `load` preserved session for `p1persist`; post-load create succeeded without re-login |
| `tests\diagnostics\06_vfs_test.txt` | PASS | Canonical VFS commands worked; deleted-file read produced expected failure |
| `tests\diagnostics\07_full_regression_test.txt` | PASS | Full diagnostic flow exited 0; manual `load` preserved session |

## Existing Script Results

All existing scripts exited with code 0, but several contain expected or legacy negative probes.

| Script | Result | Remaining output issues |
|---|---:|---|
| `tests\demo_commands.txt` | PASS with expected/legacy failures | Contains intentional `unknown_command`; also depends on old clean PID/user state |
| `tests\full_demo_commands.txt` | PASS with stale snapshot caveats | Assumes clean user/PID/memory state; manual `load` now preserves session as intended |
| `tests\01_user_test.txt` | PASS with script-format/stale snapshot issues | Leading `#` comments are treated as commands; `alice` already exists and is locked in current snapshot |
| `tests\02_process_test.txt` | PASS with script-format/stale snapshot issues | Leading `#` comments are treated as commands; hard-coded PIDs fail when `nextPid` is not 1 |
| `tests\03_memory_test.txt` | PASS with stale snapshot caveats | Memory pressure from existing snapshot can cause allocation failures |
| `tests\04_scheduler_test.txt` | PASS with stale snapshot caveats | Scheduler commands are recognized; some process setup allocations depend on current memory state |
| `tests\05_persistence_test.txt` | PASS | No command-recognition regression observed |
| `tests\06_vfs_test.txt` | PASS with script-format issue | Leading `#` comments are treated as commands |
| `tests\07_overview_test.txt` | PASS with stale snapshot caveats | Overview reports stale/foreign ready queue entries from loaded state |

## Raw Command Mode

Raw command mode was verified through redirected command scripts:

```powershell
cmd /c ".\x64\Release\OSCoreSim.exe < tests\diagnostics\07_full_regression_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\01_user_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\02_process_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\03_memory_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\04_scheduler_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\05_persistence_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\06_vfs_test.txt"
cmd /c ".\x64\Release\OSCoreSim.exe < tests\07_overview_test.txt"
```

Result: PASS. No Phase 3 scheduler aliases were rejected in raw mode.

## Menu Mode

Runtime redirected scripts do not enter menu mode by design. `ConsoleApp::isInteractiveInput()` only enables `MenuConsole` when input is real `std::cin` attached to a TTY; file redirection goes directly to raw command mode.

Static route verification from `src\app\MenuConsole.cpp` shows the menu still generates canonical commands:

| Menu area | Generated raw commands |
|---|---|
| User | `register`, `login`, `logout`, `whoami` |
| Process | `create_pcb`, `list_pcb`, `show_pcb`, `ptree`, `block_pcb`, `wakeup_pcb`, `suspend`, `resume`, `renice`, `kill_pcb`, `readyq` |
| Memory | `alloc`, `free_mem`, `show_mem`, `mem_stat`, `compact`, `set_alloc_algo`, `pgfault`, `swap_out` |
| Scheduler | `step`, `start_sched`, `stop_sched`, `restart_sched` |
| Persistence | `save`, `load` |
| Overview | `overview`, `status` |
| VFS | `touch_file`, `write_file`, `read_file`, `ls_file`, `rm_file` |

Menu command generation remains compatible with Kernel and Dispatcher routes. A true interactive menu E2E smoke still needs a console/PTY runner rather than `< file` redirection.

## Persistence Behavior

| Scenario | Result |
|---|---|
| Clean boot with no snapshot | PASS; first process starts at `PID=1` |
| Boot auto-load from existing snapshot | PASS; session starts logged out |
| Manual `load` with same current user in snapshot | PASS; session preserved |
| PID after load | PASS; `nextPid` remains monotonic and restored from snapshot |
| Snapshot preservation during Phase 4 | PASS; original `data\os_state.bin` restored after test scripts |

## Scheduler Behavior

| Command | Result |
|---|---|
| `step` | PASS |
| `start_sched` | PASS |
| `stop_sched` | PASS |
| `restart_sched` | PASS |
| `start` | PASS, alias for `start_sched` |
| `stop` | PASS, alias for `stop_sched` |
| `restart` | PASS, alias for `restart_sched` |

No `Unknown command: start`, `Unknown command: stop`, or `Unknown command: restart` appeared in the scheduler diagnostic log.

## Remaining Failures and Classification

| Symptom | Reproduction | Classification | Notes |
|---|---|---|---|
| `Unknown command: #` in `01_user_test`, `02_process_test`, `06_vfs_test` | `cmd /c ".\x64\Release\OSCoreSim.exe < tests\01_user_test.txt"` | Test script/documentation mismatch | Raw parser does not support comments. Existing scripts include `#` prose lines. |
| `Unknown command: unknown_command` in `demo_commands.txt` | `cmd /c ".\x64\Release\OSCoreSim.exe < tests\demo_commands.txt"` | Intentional negative probe | Script explicitly contains `unknown_command`. |
| `Register failed: username already exists` / locked `alice` | `cmd /c ".\x64\Release\OSCoreSim.exe < tests\01_user_test.txt"` | Stale snapshot issue | Current `data\os_state.bin` already contains users from earlier runs. |
| Process failures for hard-coded PIDs `1`, `2`, `3`, `4`, `5`, `6` | `cmd /c ".\x64\Release\OSCoreSim.exe < tests\02_process_test.txt"` and `cmd /c ".\x64\Release\OSCoreSim.exe < tests\full_demo_commands.txt"` | Stale snapshot/test assumption | PID is intentionally monotonic and restored from snapshot, so hard-coded clean-boot PIDs are not stable. |
| Allocation failures in older demo/process/memory scripts | `cmd /c ".\x64\Release\OSCoreSim.exe < tests\03_memory_test.txt"` | Stale snapshot issue | Existing snapshot and previous script actions can leave memory nearly full. |
| Overview warnings for queued PIDs not owned by current user | `cmd /c ".\x64\Release\OSCoreSim.exe < tests\07_overview_test.txt"` | Stale snapshot issue | Overview correctly reports inconsistent/foreign ready queue entries from loaded state. |

No remaining issue points to a Phase 3 code regression.

## Git and Project Layout Checks

| Check | Result |
|---|---|
| `git status` | Only Phase 3 runtime edits plus untracked diagnostics/report files are present |
| `git diff --stat` | Phase 3 source/README changes only before this report; no broad churn |
| `git diff -- src tests` | Source diff limited to Phase 3 scheduler/session fixes; tests only contain new untracked diagnostics |
| Visual Studio project files | No changes to `.sln`, `.vcxproj`, or `.filters` |
| Runtime snapshot | `data\os_state.bin` restored; not shown as modified |
| CMake files/references | No CMake files present and no CMake-named files in current diff/status |

## Branch Safety

The branch is safe to continue into the next phase.

Recommended next phase:

1. Decide whether existing legacy scripts should be normalized to avoid `#` comments and hard-coded clean PIDs.
2. Keep Phase 3 runtime code as-is; scheduler aliases and manual-load session preservation pass regression.
3. If menu mode needs automated E2E coverage, add a small PTY/console harness rather than using redirected input.
