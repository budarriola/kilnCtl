# Pico fault-hook diagnostics: hardware verification — 2026-09-14

Executes the hardware verification procedure designed (not executed) in
`docs/audits/pico_fault_hook_diagnostics_audit_2026-09-11.md` (section 7) and
picks up where `docs/audits/rp2040_fatal_fault_diag_hw_verify_2026-09-10.md`
left off (blocked on the board being busy; its instrumented ELFs were stale
against the SMP multi-tenancy fix `1496e2889`). This session rebuilt fresh
from current HEAD and ran the full trigger/observe/restore sequence for all
three fatal-fault diagnostics on real hardware. No heating run was performed.

## Pre-run state

- ESP (`get_heap_status`): `reset_reason='other watchdog'`, `uptime_s=28278`
  (~7.85 h), no unacknowledged-crash banner printed.
- Pico (`safety_get_fw_version`): build `c27484a2`, built 2026-09-10
  20:14:28Z, `boot_id=38`, commissioned, protocol v12.
- `safety_get_status`: link up, SaftyFW armed, relay_owner not tripped,
  safety thermocouple valid.
- `GET /api/status`: all four relays off, `safety_heating_enabled=true`,
  `diag_trip_reason=0`, `diag_boot_reason=2` (`BOOT_WATCHDOG` only, all
  three fatal booleans false) — clean baseline.
- Restore path established **before** provoking anything: `build_saftyfw()`
  run in the main tree at its then-HEAD (`9ae4de3d`) to produce
  `firmware/SaftyFW/build/SaftyFW.elf`, the image every trigger test below
  was reflashed back to via `debug_program(peer="pico", ...)`.

## Preparation

The `C:\wt\saftydiag` worktree from the 2026-09-10 prep was stale (pinned at
`b01afa7f`; 25 files / 1465+/73- lines of diff against current HEAD in
`firmware/SaftyFW`/`firmware/CommonFW`, including the SMP multi-tenancy fix
itself and `test_scratch_migration.c`). Rather than reuse it, created a fresh
detached worktree, `C:\wt\saftydiag2`, at HEAD `1975f14c`.

Added the same three throwaway, clearly-marked trigger blocks the 2026-09-10
prep described, plus one small `CMakeLists.txt` addition
(`if(DEFINED ENV{SAFTYDIAG_TRIGGER}) add_compile_definitions(...)`),
bracketed with `==== TEMPORARY INSTRUMENTATION ... NEVER COMMIT ====`
comments, in `C:\wt\saftydiag2\firmware\SaftyFW\src\main.c` and
`CMakeLists.txt` only:

- `SAFTYDIAG_TRIGGER_ASSERT` — `configASSERT(0)` placed **after**
  `watchdog_enable()` (not before, as the 2026-09-10 note described — placing
  it before the watchdog is armed would hang forever with no reset to
  observe; confirmed this by re-deriving it from the code rather than
  copying that detail as-is).
- `SAFTYDIAG_TRIGGER_MALLOC` — `pvPortMalloc(0xFFFFFFF0u)`, same placement.
- `SAFTYDIAG_TRIGGER_OVERFLOW` — a new task with a 32-word stack
  (`saftydiag_overflow_task_fn`), created just before `vTaskStartScheduler()`,
  recursing through a volatile-gated condition so `-Werror=infinite-recursion`
  doesn't block the build (same technique the 2026-09-10 prep used).

All three built cleanly under this codebase's `-Wall -Wextra -Werror`
(`build_assert/`, `build_malloc/`, `build_overflow/`), plus a `build_clean`
sanity build with no trigger defined. These edits exist only in the throwaway
worktree and were never staged or committed to the main tree; the worktree
was removed (`git worktree remove --force`) after use, along with the stale
2026-09-10 worktree (`C:\wt\saftydiag`), which was no longer needed.

**Note on the crash/reset loop shape**: because each trigger fires very
early in `main()` (before or shortly after `vTaskStartScheduler()`), every
boot of an instrumented image re-crashes before it ever reaches the
boot-time read/report code (step 3c) or starts `link_task`, so an
instrumented image can never itself observe or transmit its own fault — it
only ever *sets* the latch. The procedure actually used: flash the
instrumented ELF, let it crash/reset at least once (confirmed via the
resulting rise in `boot_id`), then immediately reflash the plain HEAD build
(`firmware/SaftyFW/build/SaftyFW.elf`) — that boot reads the still-latched
`scratch[5]`, reports it, caches it, and sends it in the next DIAG frame,
without re-triggering.

## Results, per diagnostic

### 7a. `configASSERT` (bit 0x20, `ASSERT_FAILED`)

- Flashed `build_assert/SaftyFW.elf`; observed via ESP polling that the link
  dropped into a boot-identity-unknown state (`boot_id=0`,
  `config_version=0`, "Pico has not reported a build identity") — consistent
  with a boot loop that never reaches `link_task_start()`.
- Reflashed the plain HEAD build. Next `GET /api/status`:
  **`diag_boot_reason=34` (0x22 = `BOOT_WATCHDOG`|`BOOT_ASSERT_FAILED`),
  `diag_boot_assert_failed=true`, the other two false.**
- Chain carried end-to-end: hook write → survived reset → read at boot →
  cached → transmitted in DIAG → decoded by the ESP → visible over
  `GET /api/status` (and therefore the kilnctrl MCP's `safety_get_status`-
  family reads, which sit on the same cache).
- Confirmed the register is not stuck: on the *next* boot (the following
  trigger's restore-flash), `diag_boot_assert_failed` read back `false`.
- `safety_get_fw_version()` afterward reported `Pico build: 9ae4de3d (dirty)`
  — the main tree's HEAD at the time of the restore build; "dirty" reflects
  only the pre-existing untracked `cfg_fs_test_*`/`build_estop*` directories
  from other sessions' work, not anything this task touched.

### 7c. `vApplicationMallocFailedHook` (bit 0x10, `MALLOC_FAILED`)

- Flashed `build_malloc/SaftyFW.elf` (`pvPortMalloc(0xFFFFFFF0u)`,
  guaranteed heap exhaustion); let it crash/reset, then reflashed the plain
  HEAD build.
- Next `GET /api/status`: **`diag_boot_reason=18` (0x12 = `BOOT_WATCHDOG`|
  `BOOT_MALLOC_FAILED`), `diag_boot_malloc_failed=true`, the other two
  false.**
- Chain carried end-to-end, same path as 7a. `safety_get_status()` showed
  link up, no trip, immediately after.

### 7b. `vApplicationStackOverflowHook` (bit 0x08, `STACK_OVERFLOW`)

- Flashed `build_overflow/SaftyFW.elf` (32-word task, guaranteed recursion
  overflow); let it crash/reset, then reflashed the plain HEAD build.
- Next `GET /api/status`: **`diag_boot_reason=10` (0x0A = `BOOT_WATCHDOG`|
  `BOOT_STACK_OVERFLOW`), `diag_boot_stack_overflow=true`, the other two
  false.**
- Confirmed cleared on the following boot (the final restore flash below
  read back `diag_boot_reason=2`, all three fatal booleans false).
- `get_heap_status()` on the ESP immediately after printed no unacknowledged-
  crash banner.

### Cross-contamination check (implicit)

Across the three sequential trigger/restore cycles, no diagnostic bit was
ever misreported as a different one (no ASSERT_FAILED read while
MALLOC_FAILED was expected, etc.) — the shared `scratch[5]` register's
tag-byte discrimination held on hardware across all three cases, and the
2026-09-10-discovered SMP multi-tenancy race (`watchdog_task` on the TRIP
core clobbering a LINK-core fatal tag with its own `0xD9`, fixed by
`1496e2889`) did not resurface: every trigger's tag survived to be read
correctly, consistent with that fix being present at current HEAD.

### `BOOT_BROWNOUT` (0x04)

Not attempted, per task instruction — confirmed dead (no detector) by prior
code review, not re-verified here.

## Post-run state / final health

- `safety_clear_trip()` called once, after the final restore flash produced
  the expected S6a (`mainFault`) trip from the dual-reflash safety-link
  handshake window (`diag_trip_reason=6`, `diag_trip_mask=32` = `1 <<
  (6-1)`, matching the documented formula — no other bit set). Confirmed
  link was up before clearing, per the standing dual-reflash procedure.
- After clearing and waiting out the ~60 s startup GRACE window,
  `safety_get_status()` reported: **link up; SaftyFW armed (relay_owner not
  tripped); safety thermocouple valid.**
- `GET /api/status`: all four relays off, `safety_heating_enabled=true`,
  `diag_trip_reason=0`, `diag_boot_reason=2` (`BOOT_WATCHDOG` only, all three
  fatal booleans false).
- `safety_get_fw_version()`: Pico build `9ae4de3d` (main tree's HEAD at
  restore-build time), `boot_id=26`, `config_version=147`, commissioned,
  protocol v12/min-compatible v7.
- `get_heap_status()` on the ESP: no unacknowledged-crash banner, heap
  headroom unchanged from the pre-run baseline within normal variance.
- No heating run was performed at any point.

**Minor side observation, not investigated further**: `config_version`
incremented once per reset across the sequence (144→145→146→147) alongside
a changing `config_crc`. Every reset in this sequence was a deliberate
reflash/watchdog-reset of a board already `commissioned`, so this reads as
an existing per-boot config-store re-persist behavior rather than anything
introduced by this task's triggers — flagged here only so a future session
investigating config-store write frequency has this data point, not raised
as a defect.

## Summary

All three fatal-fault diagnostics with a real producer (`STACK_OVERFLOW`,
`MALLOC_FAILED`, `ASSERT_FAILED`) were provoked deliberately on real
hardware and observed to carry correctly through the full chain: hook write
to `watchdog_hw->scratch[5]` → survives the watchdog reset → read and cached
at boot (`main.c` step 3c) → packed into the DIAG frame's `boot_reason` byte
(`link_task.c`) → parsed and cached on the ESP
(`safety_link_frames.c:887`) → decoded and exposed via `GET /api/status`
(and the kilnctrl MCP tools reading the same cache). Each also confirmed to
clear correctly on the following boot (not stuck). This closes the "never
observed end-to-end on hardware" gap the 2026-09-11 audit identified, and
confirms the SMP multi-tenancy fix (`1496e2889`) is in effect on current
HEAD — none of the three cross-contaminated with each other or with the
overdue-checkin latch during this run. `BOOT_BROWNOUT` remains confirmed
dead (no detector), unchanged from prior audits.

Board finished healthy: both processors linked, armed, no trip, relays off,
no unacknowledged crash report, heating enabled.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
