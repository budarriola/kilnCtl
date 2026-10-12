# Review: BUILDFX3 (c17c9ef1a), 2026-10-12

Adversarial review of `c17c9ef1a` on origin/dev ("build_kilnfw/flash/build_lock:
review BUILDFX2 fixes"). It fixes `REVIEW_BUILDFX2_2026-10-12.md` LOW-1 and
LOW-2, and it also:

- adds a 0-byte refusal to `flash_firmware`;
- makes `mcpkit/buildlock.py` reclaim a lock at once when the recorded PID is
  dead or has been reused.

Reviewed in `C:\wt\rvbuildfx3_*` at `c17c9ef1a`.

**Verdict:**

- LOW-1, LOW-2 and the 0-byte refusal are correct, and their tests bite (all 7
  claimed mutations are CAUGHT).
- The new dead-PID reclaim adds two MED defects. Both reproduced on this
  machine. In both, two writers end up in the same build directory, which is
  the failure `build_lock` exists to prevent.

## MED

### MED-1 Reclaim is check-then-act: two waiters can both hold the lock

`tools/PcTools/src/mcpkit/buildlock.py:203-211`. Waiter A runs
`_holder_is_dead(lock_path)` (a read of the content), then
`os.remove(lock_path)`. Nothing ties the file A judged to the file A removes.

The race:

1. Waiters A and B both read the dead holder's lock.
2. B removes it and `O_EXCL`-creates its own. B now holds the lock.
3. A's `os.remove` deletes B's live lock.
4. A's next `O_EXCL` create succeeds. A and B both build the same directory.

The release in `finally` (`:221`) also removes whatever file is at the path
unconditionally. So A's release can delete a third holder's lock, and the
problem cascades.

The same race already existed on the `age > stale_after` path, but that path
fires only after 35 minutes. The new path fires on every poll whenever a holder
was killed, so the window is now ordinary.

Reproduced (`race_demo.py`, scratch):

- Deterministic interleave: A acquired while B's lock file was in place, and
  B's file was replaced.
- Real 8-process stress on a dead-PID lock: **4 of 20 rounds had two
  concurrent holders.**

Fix:

- Make the reclaim atomic per judged file. Either:
  - take a sidecar `<lock>.reclaim` with `O_EXCL`, re-read and re-judge the
    content inside it, and remove only if the content is byte-identical to the
    dead holder's; or
  - `os.rename(lock_path, unique_tmp)`, then re-read `unique_tmp`. If it is not
    the judged dead content, put it back with `os.rename` (which fails if a new
    lock exists) or treat it as contention.
- Write a unique token (pid, start time, uuid) into the lock. On release, only
  remove the file if it still carries your own token.
- Add a multi-process test: N waiters on a dead-PID lock, at most one inside at
  a time.

### MED-2 The recorded PID is the MCP server, not the build: restarting the server mid-build lets a second build start alongside the orphan

The lock records `os.getpid()` (`buildlock.py:188`), which is the long-running
MCP server. The build runs as a child tree: powershell, then idf.py, ninja and
the compilers. These run from `workbench._run` (`workbench.py:188`) in a daemon
thread (`build_jobs.py:169`) when started with `build_kilnfw_start`.

Nothing kills that tree when the server process exits:

- there is no Job object (`CreateJobObject` appears nowhere in
  `tools/PcTools/src`);
- `mcp_servers.ps1` falls back to `Stop-Process` on the server PID only.

A restart mid-build is routine here. CLAUDE.md allows it with notice, and
`build_kilnfw_start` jobs run for many minutes. After such a restart:

- the orphaned ninja keeps writing `build/`;
- the lock names a dead PID;
- the restarted server's next `build_kilnfw` reclaims at once and starts a
  second ninja in the same directory.

Before this commit, the 2100 s `stale_after` covered most such builds.

Reproduced (`orphan_demo.py`, scratch): a holder took the lock, spawned a
20 s "build" child and was killed. A waiter acquired in **0.47 s**, while the
orphaned child was still alive.

The kernel build-gate mutex (`buildgate.py`) does not help. It is abandoned on
the same death, and it never serialized same-directory writers anyway.

Fix (any one):

- (a) Put the build tree in a Job object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` in `_run`, so the tree dies with the
  server.
- (b) Record the compile child's PID and start time in the lock, as
  `register_compile_pid` already does for gate records. Treat the lock as live
  while any recorded PID, or any descendant of it, is alive.
- (c) Keep the dead-PID fast path only when no process has the build dir as its
  cwd.

(a) is the simplest and also closes the existing orphan-after-stall gap.

## LOW

### LOW-1 PID-reuse guard compares against wall-clock content, not a recorded start time

`buildlock.py:139,152`. The guard reclaims when the process start time is more
than `acquired + 5 s`, where `acquired` is `time.time()` written as `%.0f`.
`GetProcessTimes` creation time and `time.time()` share the system clock, so
there is no cross-clock skew.

A backwards clock step of more than 5 s after the holder started, but before
it wrote the lock, makes a live holder look reused. That can come from W32Time
stepping a large offset, or a manual clock change. The lock is then reclaimed.
This is rare.

Fix: record the holder's own process start time in the lock, and reclaim only
when the live PID's start time differs by more than about 2 s. This is the
`buildgate.pid_alive(pid, start_epoch)` scheme (`buildgate.py:359-382`), which
should be reused rather than re-implemented: `_pid_info` duplicates
`process_start_epoch` but without `argtypes`.

### LOW-2 Three `_pid_info` branches are untested (negtest MISSED)

These own mutations were MISSED:

- **R1:** access-denied treated as dead (`buildlock.py:114`). This is the
  branch protecting a live elevated or other-user holder.
- **R4:** the `STILL_ACTIVE` check dropped (`:119`).
- **R5:** a `GetProcessTimes` failure treated as dead (`:124`).

The code itself is right in all three. Access denied means alive, a failed
`GetExitCodeProcess` or `GetProcessTimes` means alive, and an exited process
with an open handle means dead. Only the tests are missing.

Fix: unit tests that mock `ctypes.WinDLL` or the kernel32 calls:

- `OpenProcess` returning NULL with last error 5 gives alive;
- an exit code other than 259 gives dead;
- a `GetProcessTimes` failure gives alive with an unknown start time.

### LOW-3 `fixture_flash` still accepts 0-byte images

`mcp_server_flash.py:2032` checks only `os.path.isfile` before
`kill_openocd_sessions()` and the OpenOCD program step. The new refusal covers
`flash_firmware` only.

`flash_firmware`'s refusal itself is correct. At `:1293-1297` it runs:

- before any OpenOCD action (only `_find_openocd_exe`, a path lookup, precedes
  it);
- for both the default and `kiln_fw_root` trees, since `build_dir` derives from
  `effective_kiln_fw_root`.

`flash_recovery` goes through `recovery_flash.validate_image`.

Fix: apply the same `getsize <= 0` refusal to `fixture_flash`'s `images` list.

## INFO

- **`_app_image_problem` / mtime-only freshness is right.**
  - The desc time is fixed at the last recompile of `app_desc`'s TU, so it is
    the wrong signal after an incremental relink.
  - The mtime proves the link step wrote `KilnCtrl.bin` after the post-lock
    baseline. With the magic check, this catches a stale, missing, empty or
    non-image `.bin`.
  - It cannot catch a `.bin` from the wrong tree or sdkconfig that ninja wrote
    during this job. No build-side check can. That is `REVIEW_BUILDFX2` LOW-3,
    still open, and the right place for it is a cross-check in
    `flash_firmware` against the matching ELF.
- **Holder PID.** No parent-writes-then-spawns case exists. The writer is the
  process that calls `build_lock` (`workbench._run_locked`, the only Python
  caller). The PowerShell `tools/build_lock.ps1` uses a separate named kernel
  mutex plus its own records under `C:\wt\buildlocks`, not this file format.
- **Empty or unparseable lock.** It is never reclaimed early; it waits for the
  35-minute age path. This is correct (R3 CAUGHT).
- **Test structure.** `test_flash_firmware_erase_partitions.py:334`
  `EmptyBuildOutputRefusedTest` subclasses `EraseFlashFirmwareEndToEndTest`,
  so every parent test runs twice. In both edited test files, the new classes
  sit below `if __name__ == "__main__": unittest.main()`, so running the file
  directly skips them (pytest is unaffected). Use a mixin or setUp helper, and
  move the classes above the `__main__` guard.
- `_pid_info` calls `GetExitCodeProcess`, `GetProcessTimes` and `CloseHandle`
  without `argtypes`. This is harmless at today's handle values, but it is
  another reason to reuse `buildgate`'s declared prototypes.

## Negative tests

Command (in the review worktree at c17c9ef1a, main-tree PcTools venv python):

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset pytest `
  -PresetArg "tests/test_buildlock.py tests/test_build_kilnfw_artifact_gate.py tests/test_flash_firmware_erase_partitions.py" `
  -Mutations bfx3_mut.json
```

Baseline PASS (131.5 s). `real_tree_unchanged: true`, `copies_removed: true`.
Every CAUGHT is a named test failure, not an import or collection error.

| Mutation | Result |
|---|---|
| N2 case-sensitive env strip | CAUGHT (`test_clean_env_strip_is_case_insensitive`) |
| N3 real `_run_locked` skips post-lock start_wall | CAUGHT |
| N7 wrapper: no LASTEXITCODE reset | CAUGHT |
| N8 downgrade drops full log line | CAUGHT |
| F1 flash: no 0-byte refusal | CAUGHT (3 tests) |
| L1 no dead-PID reclaim | CAUGHT |
| L2 no PID-reuse reclaim | CAUGHT |
| R1 access-denied live PID treated as dead | **MISSED** (LOW-2) |
| R2 reuse slack -1e9 (every live holder reclaimed) | CAUGHT |
| R3 unparseable holder treated as dead | CAUGHT |
| R4 STILL_ACTIVE check dropped | **MISSED** (LOW-2) |
| R5 GetProcessTimes failure treated as dead | **MISSED** (LOW-2) |

The race cases (MED-1, MED-2) are not mutations: the current code already has
the defect. They were shown with the two scratch scripts described above.

## Fix status

| Finding | Status |
|---|---|
| MED-1 reclaim check-then-act | open |
| MED-2 orphaned build after server death | open |
| LOW-1 reuse guard vs clock step | open |
| LOW-2 `_pid_info` branch tests | open |
| LOW-3 `fixture_flash` 0-byte | open |

## Full -Fast suite (review worktree at c17c9ef1a)

Result: 174 passed, 17 SKIP-FAST, 6 failed.

None of the six failures involves a file this commit touches. The commit
changes only Python and docs, and none of the six is a Python/pytest check.

| Check | Cause |
|---|---|
| `check_js_host_tests.ps1` | `test_zones_type_toggle.js` (`kgRawAttrs is not defined`), the known failure |
| `check_page_js_tests.ps1` | `test_zones_type_toggle.js` (`kgRawAttrs is not defined`), the known failure |
| `check_gate_negative_test_table.ps1` | No evidence rows for `tools/check_lcd_sink_callgraph.ps1` and `tools/test_check_lcd_sink_callgraph.ps1` (added by 107412719). Fails alone too. |
| `check_main_baseline.ps1` | 9 assertions fail deterministically, also when run alone. Its stub copy of `run_all_checks.ps1` cannot find the newly dot-sourced `lib_check_discovery.ps1` (CommandNotFoundException). Dev breakage from 1ead3e7bc. |
| `check_land.ps1` | Failed under suite load. PASSES alone. |
| `test_host_fakes.ps1` | Hit the 900 s cap under load (a concurrent negtest was running). All fakes it reached were OK. |
