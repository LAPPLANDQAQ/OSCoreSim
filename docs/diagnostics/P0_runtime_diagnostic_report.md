# P0 Runtime Diagnostic Report

Date: 2026-06-09
Workspace: `C:\code\OSCoreSim`

## 1. Git and Project Baseline

- Current branch: `No_Cmake_version`
- `git status --short --branch`: `## No_Cmake_version`
- `git diff --stat`: empty
- Note: `git status` emitted warnings that `C:\Users\wth\.config\git\ignore` could not be accessed. This did not affect repository status collection.

Required Visual Studio files are present:

| File | Exists |
|---|---:|
| `OSCoreSim.sln` | yes |
| `OSCoreSim/OSCoreSim.vcxproj` | yes |
| `OSCoreSimTests/OSCoreSimTests.vcxproj` | yes |

## 2. Runtime Snapshot

`data/os_state.bin` exists.

Read-only metadata inspection of the snapshot:

| Field | Value |
|---|---|
| Magic | `OSSM2026` |
| Version | `2` |
| Users | `alice,bob` |
| `nextPid` | `9` |
| PCB count | `3` |
| PCB PIDs | `5,7,8` |
| Ready Q0 | empty |
| Ready Q1 | empty |
| Ready Q2 | `8,7` |
| Total memory | `1024 KB` |
| Allocation algorithm enum | `2` |
| Memory block count | `9` |
| Scheduler running | `false` |
| Scheduler owner | `bob` |
| Next file id | `1` |
| VFS file count | `0` |

## 3. Build Validation

MSBuild was found via `vswhere`:

`C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe`

Initial sandboxed Debug build reached MSBuild but failed because Windows SDK discovery could not access `C:\Users\wth\AppData\Local\Microsoft SDKs`. After running the requested build with approved elevated access:

| Command | Result |
|---|---|
| `msbuild OSCoreSim.sln /p:Configuration=Debug /p:Platform=x64` | success, 0 warnings, 0 errors |
| `msbuild OSCoreSim.sln /p:Configuration=Release /p:Platform=x64` | success, 0 warnings, 0 errors |

## 4. Command Mapping Table

Status meanings:

- OK: command name is consistently routed.
- mismatch: names differ across command surfaces.
- missing: surface does not expose the command directly.

| User-facing command | Menu command | Raw command | Kernel route | Dispatcher route | Status |
|---|---|---|---|---|---|
| `help` | raw-mode only | `help` | falls through | `helpText()` | missing in menu |
| `status` | `status` | `status` | falls through | `statusText()` | OK |
| `clear` | raw-mode only | `clear` | falls through | `clear` branch | missing in menu |
| `exit` | `exit` in Master menu exit | `exit` | falls through | `exit` branch | OK |
| `quit` | raw-mode only | `quit` | falls through | `quit` branch | missing in menu |
| `register <username> <password>` | `register ...` | `register ...` | falls through | `registerUser()` | OK |
| `login <username> <password>` | `login ...` | `login ...` | falls through | `login()` | OK |
| `logout` | `logout` | `logout` | falls through; stops scheduler after success | `logout()` | OK |
| `whoami` | `whoami` | `whoami` | falls through | `whoami()` | OK |
| `create_pcb <name> <memKB> <priority> <totalTime> [ppid]` | `create_pcb ...` | `create_pcb ...` | falls through | creates memory, then PCB | OK |
| `kill_pcb <pid>` | `kill_pcb ...` | `kill_pcb ...` | falls through | kill subtree, then frees process memory | OK |
| `block_pcb <pid>` | `block_pcb ...` | `block_pcb ...` | falls through | `blockProcess()` | OK |
| `wakeup_pcb <pid>` | `wakeup_pcb ...` | `wakeup_pcb ...` | falls through | `wakeupProcess()` | OK |
| `show_pcb <pid>` | `show_pcb ...` | `show_pcb ...` | falls through | `showProcess()` | OK |
| `list_pcb` | `list_pcb` | `list_pcb` | falls through | `listProcesses()` | OK |
| `ptree` | `ptree` | `ptree` | falls through | `processTree()` | OK |
| `suspend <pid>` | `suspend ...` | `suspend ...` | falls through | `suspendProcess()` | OK |
| `resume <pid>` | `resume ...` | `resume ...` | falls through | `resumeProcess()` | OK |
| `renice <pid> <newPriority>` | `renice ...` | `renice ...` | falls through | `reniceProcess()` | OK |
| `readyq` | `readyq` | `readyq` | falls through | `readyQueueSnapshot()` | OK |
| `alloc <sizeKB>` | not generated | `alloc <sizeKB>` | falls through | `allocateManual(owner, manual, size)` | missing in menu |
| `alloc <name> <sizeKB>` | `alloc <name> <sizeKB>` | `alloc <name> <sizeKB>` | falls through | `allocateManual(owner, tag, size)` | OK |
| `free_mem <addr>` | `free_mem ...` | `free_mem ...` | falls through | `freeByAddress()` | OK |
| `show_mem` | `show_mem` | `show_mem` | falls through | `showMemory()` | OK |
| `compact` | `compact` | `compact` | falls through | `compact()`, then PCB memStart sync | OK |
| `mem_stat` | `mem_stat` | `mem_stat` | falls through | `memoryStat()` | OK |
| `set_alloc_algo <FF|BF|WF>` | `set_alloc_algo ...` | `set_alloc_algo ...` | falls through | `setAlgorithm()` | OK |
| `pgfault [pid]` | `pgfault` or `pgfault <pid>` | `pgfault` or `pgfault <pid>` | falls through | simulated page fault | OK |
| `swap_out <pid>` | `swap_out ...` | `swap_out ...` | falls through | memory swap out, then PCB mark swapped | OK |
| `start_sched` | `start_sched` | `start_sched` | `isSchedulerCommand()` | not reached | OK |
| `stop_sched` | `stop_sched` | `stop_sched` | `isSchedulerCommand()` | not reached | OK |
| `restart_sched` | `restart_sched` | `restart_sched` | `isSchedulerCommand()` | not reached | OK |
| `step` | `step` | `step` | `isSchedulerCommand()` | not reached | OK |
| `save` | `save` | `save` | `isPersistenceCommand()` | not reached; defensive branch exists | OK |
| `load` | `load` | `load` | `isPersistenceCommand()` | not reached; defensive branch exists | OK |
| `overview` | `overview` | `overview` | `isVisualizationCommand()` | not reached | OK |
| `touch_file <name>` | `touch_file ...` | `touch_file ...` | `isVfsCommand()` | not reached | OK |
| `write_file <name> <content>` | `write_file ...` | `write_file ...` | `isVfsCommand()` | not reached | OK |
| `read_file <name>` | `read_file ...` | `read_file ...` | `isVfsCommand()` | not reached | OK |
| `ls_file` | `ls_file` | `ls_file` | `isVfsCommand()` | not reached | OK |
| `rm_file <name>` | `rm_file ...` | `rm_file ...` | `isVfsCommand()` | not reached | OK |

## 5. Command Name Mismatch Findings

No active command-name mismatch was found between the README command table, dispatcher help text, `MenuConsole` generated command strings, `Kernel` special routing, and `CommandDispatcher` branches.

Potential confusion points:

- The menu does not directly generate `help`, `clear`, `quit`, or raw `alloc <sizeKB>`. These commands are still available in raw command mode.
- In interactive terminals, the app starts in numeric menu mode. Typing a raw command such as `start_sched` at the main menu is treated as an invalid menu choice until option `8` enters raw command mode.
- Menu mode appends extra display commands after some operations:
  - process actions append `list_pcb`
  - manual memory allocation appends `show_mem` and `list_pcb`
- The README prose and some test descriptions use shorthand wording such as `start/stop/restart`, `touch/write/read/ls/rm`, and `create/kill/show`. The actual command table and code use `start_sched`, `stop_sched`, `restart_sched`, `touch_file`, `write_file`, `read_file`, `ls_file`, `rm_file`, `create_pcb`, `kill_pcb`, and `show_pcb`.

## 6. Suspected Root Causes

### PID starts from 9 or another non-1 value

Likely cause: persisted runtime state is being auto-loaded.

Evidence:

- `data/os_state.bin` exists.
- Snapshot metadata has `nextPid = 9` and existing PIDs `5,7,8`.
- `Kernel::start()` resets to a clean state, then auto-loads `data/os_state.bin` if it exists.
- `Kernel::importSnapshotLocked()` imports `snapshot.nextPid` into `ProcessManager`.
- `ProcessManager::createProcessWithMemory()` assigns `pcb.pid = nextPid_++`.

Impact: scripts or demos that assume the first new process is PID 1 will fail after loading this snapshot.

### Current user is cleared after load

Likely cause: this is explicit behavior, not an accidental loss.

Evidence:

- `Kernel::importSnapshotLocked()` calls `userManager_.clearCurrentSession()`.
- `UserManager::importUsers()` also resets `currentUser_`.
- Startup and manual load messages instruct the user to log in again.

Impact: any process, memory, scheduler, overview, or VFS command immediately after load will fail until `login <username> <password>` succeeds.

### Scheduler commands not recognized

Likely causes:

- The recognized raw scheduler names are `start_sched`, `stop_sched`, `restart_sched`, and `step`; generic `start`, `stop`, or `restart` are not command names.
- In interactive menu mode, raw scheduler command text is not accepted at the numeric menu prompt. Use scheduler menu choices or enter raw mode with option `8`.
- Scheduler commands require a logged-in user.

Routing itself appears correct: `Kernel::isSchedulerCommand()` recognizes all four scheduler commands before dispatching to `CommandDispatcher`.

### Process commands failing

Likely causes:

- Process commands require login.
- Hard-coded parent PIDs in scripts are stale after snapshot auto-load. Example: `create_pcb shell 128 5 12 1` fails unless PID 1 exists and belongs to the current user.
- The current snapshot has PIDs `5,7,8`, not `1,2,3`.
- Process creation first allocates memory. Fragmented or exhausted memory can cause `create_pcb` to fail before the PCB is created.

### Memory commands failing

Likely causes:

- Memory commands require login.
- `free_mem <addr>` only frees manual `KERNEL` blocks owned by the current user. It cannot free process memory; process memory must be released by `kill_pcb` or `swap_out`.
- Hard-coded addresses such as `free_mem 0` or `free_mem 100` depend on a clean memory layout. The loaded snapshot has 9 memory blocks, so these addresses may no longer match manual blocks for the current user.
- `set_alloc_algo` accepts only `FF`, `BF`, or `WF`.
- `alloc <name> <sizeKB>` requires a valid tag and positive integer size.

### VFS commands failing

Likely causes:

- VFS commands require login.
- The active command names include the `_file` suffix. Generic `touch`, `write`, `read`, `ls`, and `rm` are not command names.
- File names must be 1 to 64 characters and contain only letters, digits, underscore, hyphen, or dot.
- VFS is user-isolated by owner.
- The current snapshot metadata has `VfsCount = 0`, so reads/lists may be empty until files are created.

### Menu mode behaves differently from raw command mode

Likely causes:

- Interactive `stdin` starts in numeric menu mode; redirected/scripted input skips the menu and enters raw command mode.
- Menu mode generates raw commands internally, but also adds auxiliary display commands for process and memory workflows.
- Menu mode validates some fields before submitting commands.
- Client menu/raw mode sends commands to the Master through Named Pipe, while Master submits directly to `Kernel`.
- Client `exit`/`quit` in raw mode is local to the client and is not sent to the Master.

## 7. Recommended Next Phase

Phase 1 should be a controlled reproduction phase, still without broad refactoring:

1. Decide whether diagnostics should run against the existing persisted snapshot or against a clean runtime state. The current snapshot materially changes PID, memory, scheduler, and VFS behavior.
2. For snapshot-preserving tests, update test inputs to avoid hard-coded PID assumptions and use actual PIDs from command output.
3. For clean-state tests, use an explicit isolated snapshot path or an approved backup/restore workflow rather than deleting runtime data.
4. Add a small command compatibility matrix to the test plan covering raw mode, menu mode, and redirected input.
5. Re-run targeted process, memory, scheduler, VFS, and persistence scenarios after choosing the state policy.

## 8. No Source Code Change Confirmation

No files under `src/` were modified.

No files under `tests/` were modified.

No `.sln`, `.vcxproj`, or `.filters` files were modified.

CMake was not reintroduced.

No runtime data was deleted or edited.

The only intentional repository content change for this phase is this diagnostic report file.
