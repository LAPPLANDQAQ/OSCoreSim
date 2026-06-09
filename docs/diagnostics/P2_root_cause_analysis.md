# P2 Root Cause Analysis

Date: 2026-06-09
Workspace: `C:\code\OSCoreSim`

## Summary

No temporary logging was needed. The Phase 1 scripts and the requested code paths are enough to identify root causes.

There is no evidence that a broad rewrite, module refactor, CMake reintroduction, Visual Studio project layout change, or snapshot format change is needed.

## Failure Table

| Failure | Evidence | Root cause | Classification | Phase 3 direction |
|---|---|---|---|---|
| First new PID is `9`, not `1`, from repo root | P0 snapshot metadata: `nextPid = 9`, existing PIDs `5,7,8`; P1 current-snapshot run created `PID=9` | Existing `data/os_state.bin` is auto-loaded and restores `nextPid` | stale snapshot issue / intended persistence behavior | Add test isolation or documented cleanup workflow; do not change PID allocator |
| Clean boot PID expectation differs | P1 no-snapshot temp run created `PID=1` | Clean boot only occurs when no snapshot exists in working directory | intended behavior | Document clean-state precondition |
| `whoami` after `load` returns `not logged in` | P1 persistence run: `load` then `whoami` -> `not logged in` | Snapshot import clears current session | intended behavior, possible UX mismatch | Decide whether manual `load` should preserve same-user session |
| `list_pcb` / `create_pcb` after `load` fail before login | P1 persistence run: both returned `This command requires login` | Same session clear after `load` | intended behavior, possible UX mismatch | If preserving manual session, change only Kernel/UserManager session handling |
| `start`, `stop`, `restart` are unknown | P1 scheduler run: all three returned `Unknown command` | Only `start_sched`, `stop_sched`, `restart_sched`, `step` are routed | command alias missing or docs/test shorthand mismatch | Add aliases if compatibility is desired; otherwise update docs/tests |
| `show_pcb 1`, `pgfault 1`, `swap_out 1` fail | P1 process/memory runs under snapshot | Hard-coded PID 1 does not exist for current diagnostic user when `nextPid=9` | stale snapshot issue / test assumption | Use actual PIDs or isolated clean state |
| `free_mem 0` fails | P1 memory run: address 0 is free in loaded snapshot | Hard-coded address does not match current owned manual block | stale snapshot issue / test assumption | Use actual allocation output or isolated clean state |
| VFS generic names such as `touch`, `read`, `ls`, `rm` are absent | README prose uses shorthand; command table, help, menu, Kernel use `_file` names | Prose shorthand does not match canonical command names | documentation mismatch; optional compatibility alias | Prefer documentation correction unless legacy scripts require aliases |

## Area A - PID and Persistence

### Evidence

- `ProcessManager::nextPid_` defaults to `1` in `src/process/ProcessManager.h`.
- `ProcessManager::createProcessWithMemory()` assigns `pcb.pid = nextPid_++`.
- `ProcessManager::exportNextPid()` returns the current allocator value.
- `ProcessManager::importNextPid()` restores `nextPid_` with a lower bound of `1`.
- `ProcessManager::importPcbs()` also rebuilds `nextPid_` as `max(existingPid + 1)`, then `Kernel::importSnapshotLocked()` overwrites it with `snapshot.nextPid`.
- `Kernel::exportSnapshotLocked()` writes `snapshot.nextPid = processManager_.exportNextPid()`.
- `Kernel::importSnapshotLocked()` calls `processManager_.importNextPid(snapshot.nextPid)`.
- `SnapshotStore::save()` writes `snapshot.nextPid`; `SnapshotStore::load()` reads it back.
- P0 metadata inspection found `data/os_state.bin` with `nextPid = 9`.

### Answers

1. Yes. PID starting from `9` is caused by existing `data/os_state.bin` when running from the repository root.
2. Yes. `nextPid` is persisted and restored from the snapshot.
3. Yes. The Phase 1 clean temp working-directory run with no snapshot created `PID=1`.
4. This is intended persistence behavior, not a PID allocator bug.
5. Prefer a test cleanup/isolation step and documentation. A reset command is optional convenience, but not required to fix the root cause and would introduce state-destructive behavior that needs careful confirmation.

### Minimal Phase 3 Plan

- Do not change `ProcessManager` PID logic.
- Add documentation that repo-root runs auto-load `data/os_state.bin`.
- For tests, run clean-state cases from a temp working directory or backup/restore the snapshot around tests.
- If an interactive reset feature is desired, design it explicitly as a separate command with confirmation; do not fold it into existing commands.

## Area B - Session Cleared After Load

### Evidence

- `Kernel::handlePersistenceCommand()` stops the scheduler before manual `load`, reads the snapshot, then calls `importSnapshotLocked()`.
- `Kernel::importSnapshotLocked()` disables scheduler state and calls `userManager_.clearCurrentSession()`.
- `UserManager::importUsers()` also resets `currentUser_`.
- `UserManager::clearCurrentSession()` directly resets `currentUser_`.
- Startup auto-load uses the same import path and boot message says to log in.

### Answers

1. Yes. Current session is intentionally cleared after manual `load`.
2. Product decision: manual `load` can reasonably preserve the session if the same username exists in the loaded snapshot and the account is usable. This would make interactive persistence less surprising while still preserving account isolation.
3. Boot auto-load should still require login. Restoring a login automatically at program startup would bypass authentication after restart.
4. Minimal code change if Phase 3 chooses preservation:
   - Capture `currentUser` before manual load.
   - Import the snapshot.
   - Restore the session only for manual `load` and only if the same user exists in the imported users.
   - Keep startup auto-load clearing the session.

### Minimal Phase 3 Plan

Exact files if preserving manual load session:

- `src/kernel/Kernel.cpp`
- `src/auth/UserManager.h`
- `src/auth/UserManager.cpp`
- `tests/diagnostics/05_persistence_test.txt` or a new focused diagnostic script to assert chosen behavior
- `README.md` for the persistence/session rule

Possible implementation shape:

- Add a narrow `UserManager` helper such as `restoreSessionIfUserExists(const std::string&)`.
- Add an optional preserve-session path for manual `load`; do not change boot auto-load.

If the current behavior is chosen as intended, Phase 3 only updates docs/tests.

## Area C - Scheduler Recognition

### Evidence

- `Kernel::isSchedulerCommand()` recognizes:
  - `start_sched`
  - `stop_sched`
  - `restart_sched`
  - `step`
- `Kernel::handleSchedulerCommand()` implements exactly those four names.
- `CommandDispatcher::helpText()` lists exactly those four names.
- `MenuConsole::handleSchedulerMenu()` generates exactly those four names.
- README scheduler command table documents exactly those four names.
- Some prose/test labels say `step/start/stop/restart`, which can be read as shorthand.

### Answers

1. Recognized scheduler commands: `step`, `start_sched`, `stop_sched`, `restart_sched`.
2. Documented scheduler commands in the command table: `step`, `start_sched`, `stop_sched`, `restart_sched`.
3. Menu-generated scheduler commands: `step`, `start_sched`, `stop_sched`, `restart_sched`.
4. Yes. Short aliases `start`, `stop`, and `restart` are missing.
5. Recommended: add aliases if user-facing compatibility matters, because Phase 1 explicitly probes them and README prose already uses shorthand. Otherwise, update prose/tests to use only canonical names.

### Minimal Phase 3 Plan

Exact files if adding aliases:

- `src/kernel/Kernel.cpp`
- `src/kernel/CommandDispatcher.cpp`
- `README.md`
- `tests/diagnostics/04_scheduler_test.txt` or a focused scheduler alias test

Implementation should map aliases internally:

- `start` -> `start_sched`
- `stop` -> `stop_sched`
- `restart` -> `restart_sched`

No scheduler algorithm changes are needed.

## Area D - Process and Memory

### Evidence

Process command branches in `CommandDispatcher::dispatch()` cover:

- `create_pcb <name> <memKB> <priority> <totalTime> [ppid]`
- `kill_pcb <pid>`
- `block_pcb <pid>`
- `wakeup_pcb <pid>`
- `show_pcb <pid>`
- `list_pcb`
- `ptree`
- `suspend <pid>`
- `resume <pid>`
- `renice <pid> <newPriority>`
- `readyq`

Memory command branches cover:

- `alloc <sizeKB>`
- `alloc <name> <sizeKB>`
- `free_mem <addr>`
- `show_mem`
- `mem_stat`
- `set_alloc_algo <FF|BF|WF>`
- `compact`
- `pgfault [pid]`
- `swap_out <pid>`

`MenuConsole` generates the same names and argument order. The menu's manual memory flow intentionally uses `alloc <name> <sizeKB>` and then adds `show_mem` and `list_pcb` as extra display commands.

### Answers

1. Yes. Process and memory command names are consistent across README command table, help text, menu generation, and dispatcher branches.
2. Yes. Argument orders are consistent.
3. Yes. README command table matches implementation. Some section summaries use shorthand wording like `create/kill` or `alloc/free/algo`, but the command table is canonical.
4. Yes. Menu input generates valid raw commands. Menu mode can appear different because it appends follow-up display commands after process and memory actions.

### Minimal Phase 3 Plan

No process or memory code fix is indicated for command routing.

Exact files if improving test stability/docs:

- `README.md`
- `tests/diagnostics/02_process_test.txt`
- `tests/diagnostics/03_memory_test.txt`
- `docs/diagnostics/P1_regression_matrix.md` only if the diagnostic matrix is updated

Use actual PIDs/addresses or isolated clean-state setup. Do not change process/memory business logic for these failures.

## Area E - VFS

### Evidence

- `Kernel::isVfsCommand()` recognizes:
  - `touch_file`
  - `write_file`
  - `read_file`
  - `ls_file`
  - `rm_file`
- `Kernel::handleVfsCommand()` implements exactly those names.
- `CommandDispatcher::helpText()` lists exactly those names.
- `MenuConsole` generates exactly those names.
- README VFS command table lists exactly those names.
- README prose and comments sometimes use shorthand `touch/write/read/ls/rm`.

### Answers

1. Yes. VFS command names are consistent across the active command surfaces.
2. Old aliases like `touch`, `read`, `ls`, and `rm` are not currently expected by the authoritative command table, help text, menu, or scripts. They are only implied by shorthand prose.
3. Prefer documentation cleanup rather than compatibility aliases unless there are known legacy scripts or grading inputs that use the short names. If compatibility is required, aliases can be added narrowly in `Kernel` without changing VFS storage logic.

### Minimal Phase 3 Plan

Recommended documentation-only path:

- `README.md`

Optional alias path:

- `src/kernel/Kernel.cpp`
- `src/kernel/CommandDispatcher.cpp`
- `README.md`
- `tests/diagnostics/06_vfs_test.txt` or a focused VFS alias test

Alias mapping, if chosen:

- `touch` -> `touch_file`
- `write` -> `write_file`
- `read` -> `read_file`
- `ls` -> `ls_file`
- `rm` -> `rm_file`

No snapshot format or VFS data model change is needed.

## Root Cause Classification

| Issue | Type |
|---|---|
| PID starts at `9` under repo root | stale snapshot issue and intended behavior |
| Clean boot starts at `1` only without snapshot | intended behavior |
| Manual `load` clears current user | intended behavior, possible UX policy mismatch |
| Boot auto-load requires login | intended behavior |
| `start` / `stop` / `restart` unknown | command alias missing or documentation/test shorthand mismatch |
| Process PID-1 probes fail | stale snapshot issue / test assumption |
| Memory address/PID probes fail | stale snapshot issue / test assumption |
| Process and memory command names/arguments | OK |
| VFS canonical command names | OK |
| VFS short prose names | documentation mismatch unless legacy aliases are required |

## Exact Files to Change in Phase 3

Required only if Phase 3 chooses the recommended minimal compatibility improvements:

| Goal | Files |
|---|---|
| Preserve same-user session after manual `load` only | `src/kernel/Kernel.cpp`, `src/auth/UserManager.h`, `src/auth/UserManager.cpp`, `README.md`, `tests/diagnostics/05_persistence_test.txt` or new focused test |
| Add scheduler aliases | `src/kernel/Kernel.cpp`, `src/kernel/CommandDispatcher.cpp`, `README.md`, `tests/diagnostics/04_scheduler_test.txt` or new focused test |
| Clarify clean-state testing and snapshot behavior | `README.md`, possibly `docs/visual_studio_运行说明.md`, diagnostic test notes |
| Clarify VFS canonical names | `README.md` |
| Optional VFS short aliases | `src/kernel/Kernel.cpp`, `src/kernel/CommandDispatcher.cpp`, `README.md`, VFS alias diagnostic test |

Files that should not need Phase 3 changes for these root causes:

- `src/process/ProcessManager.cpp`
- `src/process/ProcessManager.h`
- `src/persistence/SnapshotStore.cpp`
- `src/persistence/SnapshotStore.h`
- Visual Studio solution/project files
- binary snapshot format

## Minimal Fix Plan

1. Keep PID persistence as-is. Add a documented clean-test workflow instead of changing allocator behavior.
2. Decide session policy:
   - If current behavior is accepted, update docs/tests only.
   - If interactive manual `load` should preserve a valid same-user session, add the narrow UserManager helper and change only manual load flow.
3. Add scheduler aliases `start`, `stop`, `restart` if compatibility is expected.
4. Do not change process or memory command routing; update tests to avoid stale PID/address assumptions.
5. For VFS, update shorthand prose to canonical names; add aliases only if legacy command input must be supported.

## No Broad Refactor Needed

No broad refactor is needed.

No module rewrite is needed.

No binary snapshot format change is needed.

No Visual Studio project layout change is needed.

No CMake restoration is needed.

No temporary logging or source-code instrumentation was added in this phase.
