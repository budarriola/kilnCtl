# RP2040 fatal-fault diagnostics: hardware verification (2026-09-10)

## Status: PREP COMPLETE BUT STALE, ON-HARDWARE TRIGGER TESTS BLOCKED — board busy

A concurrent session fixed a real bug in this exact feature while this audit
was running (see "Multi-tenancy claim" below) — the three instrumented ELFs
built this session predate that fix and must be rebuilt from current HEAD
before use.

The Pico safety processor is not touchable right now: a ~3.3-hour `cplval75`
heating capture was running on the main board when this task started and was
still ~7.5 minutes into it (target 36.6C, all zones climbing) as of the last
check. Per this task's own safety constraint, no flash/reset/command was sent
to either processor while that capture is live. This document records what
was verified by code review and what was prepared for the on-hardware pass;
the actual trigger/observe/restore sequence below is **not yet performed**
and needs a follow-up session once the board is idle.

## Claims verified against source (31741ece, current HEAD 521006fc)

All match the commit message:

- `vApplicationMallocFailedHook()` (`firmware/SaftyFW/src/main.c:147-175`)
  writes `WATCHDOG_FATAL_MALLOC_WORD()` = `0xB4000000` to
  `watchdog_hw->scratch[5]`, then disables interrupts and hangs. No payload
  beyond the magic byte (FreeRTOS's hook takes no argument).
- `configASSERT` (`firmware/SaftyFW/FreeRTOSConfig.h:209-229`) is redefined to
  write `SAFTYFW_CONFIGASSERT_WORD(SAFTYFW_ASSERT_FILE_ID, __LINE__)` =
  `0xA5000000 | (file_id&0xFF)<<16 | (line&0xFFFF)` via a **literal MMIO
  address** (`WATCHDOG_BASE + WATCHDOG_SCRATCH5_OFFSET`), not
  `watchdog_hw->scratch[5]` — confirmed the comment's claim that including
  `hardware/watchdog.h` from `FreeRTOSConfig.h` broke the build (pico_time's
  TU reaches it before `__force_inline` is defined). `SAFTYFW_ASSERT_FILE_ID`
  defaults to 0 (`FreeRTOSConfig.h:210`), opt-in per translation unit.
- Slot 5 is confirmed multi-tenant by construction: `hal_scratch_claim(5, ...)`
  is called four times in `main()` (`main.c:283-291`) for `0xD9`
  (`watchdog_overdue_diag`), `0xE3` (`stack_overflow_hook`), `0xB4`
  (`malloc_fail_hook`), `0xA5` (`assert_hook`) — see "Multi-tenancy" below for
  why this is safe.
- Boot-time banner (`main.c:398-422`, step 3c): reads `watchdog_fatal_diag`
  before clearing, prints one of three `console_uart_puts()` messages
  (`"!!! last boot: STACK OVERFLOW, task '%c%c...'"`,
  `"!!! last boot: MALLOC FAILED (heap exhausted)"`,
  `"!!! last boot: configASSERT FAILED, file_id=%u line=%u"`) on UART0
  (GP16/GP17, the debug-probe bridge, 115200 8N1, no JTAG needed to read).
- ESP-side passthrough confirmed end to end:
  `firmware/SaftyFW/src/tasks/link_task.c:981-1058` packs
  `KILNLINK_DIAG_BOOT_STACK_OVERFLOW/MALLOC_FAILED/ASSERT_FAILED` bits into
  `boot_reason` byte (offset 10 of the DIAG frame) alongside the existing
  `KILNLINK_DIAG_BOOT_WATCHDOG`/`POWERON` bits →
  `safety_link_frames.c:883` (`link->cached.diag_boot_reason = p[10]`) →
  `dashboard_http.c:315` → `dashboard_status_http.c:498-512` emits
  `"diag_boot_reason"` plus three decoded `"diag_boot_stack_overflow"` /
  `"diag_boot_malloc_failed"` / `"diag_boot_assert_failed"` booleans in the
  ESP's dashboard JSON. No `KILNLINK_PROTOCOL_VERSION` bump, no frame-length
  change, matches the commit's claim.

## Multi-tenancy claim — WAS FALSE under SMP; fixed by a concurrent session mid-audit

Initial pass here reasoned (incorrectly) that the four scratch[5] writers are
mutually exclusive by construction because each fatal hook disables
interrupts and hangs before/at its write. That reasoning is single-core only,
and this firmware is `configNUMBER_OF_CORES 2`: `watchdog_task` is pinned to
`SAFTYFW_CORE_TRIP_PATH` while every other task (`link_task`, `log_task`,
`update_task`, the app tasks generally) is pinned to
`SAFTYFW_CORE_LINK_PATH`. A fatal hook firing on the LINK core hangs *that
core only* — the TRIP core keeps running `watchdog_task`, which notices the
hung core missed its check-in and calls `watchdog_overdue_diag_mark()`,
which used to unconditionally overwrite scratch[5] with its own `0xD9` tag,
clobbering the real fatal tag ~700ms before the watchdog reset. The board
would reset the same way either way, but the *evidence* — the banner and the
DIAG bits this whole feature exists to produce — would read "check-in
overdue" instead of naming the actual fault kind and line. This is exactly
the failure mode this feature is meant to prevent, just one level removed
(the diagnostic itself losing its own evidence to a race no one had reasoned
about across cores).

**This was found and fixed by a separate, concurrent session while this audit
was in progress** — commit `1496e2889c4e58bb0a6a2b4bbf003103189011a6`,
landed on `main` mid-task (HEAD moved from `521006fc` to `1496e288` between
this audit's earlier code-review pass and its write-up). The fix:
`watchdog_overdue_diag_mark()` now reads scratch[5] back first via
`watchdog_fatal_diag_decode()` and returns without writing if a fatal tag is
already latched; a new `watchdog_overdue_diag_notify_recovered()` also
avoids leaving a stale `0xD9` latch behind for an unrelated future reset to
misreport when an overdue condition self-recovers before the watchdog fires.
Host-tested (`test_scratch_migration.c`, negative-tested per this project's
standing rule — the guard was disabled by hand and both new checks confirmed
RED before restoring): 2494/2494 checks passed. `main.c`'s
`vApplicationStackOverflowHook()` comment, which asserted the same false
single-core mutual-exclusion claim, was corrected in the same commit.

**Implication for the hardware pass below**: this makes the malloc-fail and
assert triggers *more* interesting to actually run on hardware, not less —
before this fix, either one had a real chance of being silently misreported
as "check-in overdue" if `watchdog_task` on the TRIP core happened to notice
the hang before the reset. The three instrumented images built this session
were built from a worktree pinned to `b01afa7f`, which predates
`1496e2889` — **they must be rebuilt from current HEAD before flashing** so
the hardware pass exercises the fixed code, not the version with the bug
this session's own review just found. (SaftyFW/CommonFW sources changed
between `b01afa7f` and `1496e2889`, unlike the earlier `521006fc` diff
checked above — this diff is non-empty.)

One remaining item, not a defect: the write itself is not atomic against a
reset occurring mid-write, but a single 32-bit MMIO store is atomic on this
core, so this is a non-issue in practice — checked, not a gap.

## Preparation completed this session

1. Clean detached worktree at `C:\wt\saftydiag`, `git worktree add --detach`
   at HEAD `b01afa7f` (the tip when the worktree was created; confirmed via
   `git diff --stat b01afa7f 521006fc -- firmware/SaftyFW firmware/CommonFW`
   that these two directories are byte-identical between that commit and the
   current main-tree HEAD `521006fc`, so the worktree's SaftyFW content is
   current).
2. Three throwaway, clearly-marked trigger blocks added to
   `C:\wt\saftydiag\firmware\SaftyFW\src\main.c` and one small
   `CMakeLists.txt` addition (an `if(DEFINED ENV{SAFTYDIAG_TRIGGER})
   target_compile_definitions(...)` gate), all bracketed with
   `==== TEMPORARY INSTRUMENTATION ... NEVER COMMIT ====` comments. **These
   edits exist only in the throwaway worktree and were never staged or
   committed; the main tree at `c:\Users\budar\OneDrive\Desktop\kilnCtl` was
   never touched.**
   - `SAFTYDIAG_TRIGGER_ASSERT`: calls `configASSERT(0)` early in `main()`,
     right after the scratch claims, before the watchdog is armed.
   - `SAFTYDIAG_TRIGGER_MALLOC`: calls `pvPortMalloc(0xFFFFFFF0u)` in the same
     spot — an allocation request larger than the whole heap, guaranteed to
     fail and invoke the malloc-failed hook.
   - `SAFTYDIAG_TRIGGER_OVERFLOW`: starts one extra FreeRTOS task
     (`saftydiag_overflow_task`, 32-word stack) just before
     `vTaskStartScheduler()`; the task recurses immediately, each frame
     burning a 64-byte volatile buffer, guaranteed to blow its tiny stack
     within a few frames. (Recursion is deliberately gated on a
     non-constant volatile so `-Werror=infinite-recursion`/
     `-Werror=unused-but-set-variable` don't block the build — two small
     iterations were needed to get a clean build under this codebase's
     `-Wall -Wextra -Werror`.)
3. Three instrumented ELFs built successfully (toolchain: `arm-none-eabi-gcc
   14.2` / Ninja / `PICO_SDK_PATH=C:/pico-tools/pico-sdk`, same as the
   project's existing `build/` directory):
   - `C:\wt\saftydiag\firmware\SaftyFW\build_assert\SaftyFW.elf`
   - `C:\wt\saftydiag\firmware\SaftyFW\build_malloc\SaftyFW.elf`
   - `C:\wt\saftydiag\firmware\SaftyFW\build_overflow\SaftyFW.elf`
   A fourth, `build_clean` (no trigger define), was also built in the
   worktree as a sanity check, but the **actual restore-to-HEAD flash at the
   end of the real test must use the main tree's own `build_saftyfw()` MCP
   tool output**, not this worktree copy, so the flashed artifact's embedded
   build identity (commit hash) reflects the true main-tree HEAD at flash
   time rather than the worktree's pinned base commit.

## What is still required (blocked on the board going idle)

For each of the three ELFs above, in order:

1. `debug_program(peer="pico", elf_path="C:\\wt\\saftydiag\\firmware\\SaftyFW\\build_<x>\\SaftyFW.elf", confirm=true)`.
2. Open a serial terminal on the debug probe's COM port (probe serial
   `E66540F0A36C6E21`, historically COM10) at 115200 8N1 and capture the
   **next** boot's banner (the triggering boot itself crashes before it can
   print a banner for its own fault — the banner is printed by the boot
   *after*, which reads the latch the crashing boot left in scratch[5]).
   Since these firmware images crash deterministically at/near boot every
   time, the watchdog will keep resetting the board into the same fault
   repeatedly; the very first reset after the initial flash is the one whose
   *next* boot shows the banner (steady state after that is "crash, print
   banner, crash again" in a loop, which is itself useful confirmation but
   should be interrupted quickly by reflashing away from it).
3. Read `safety_get_status()`/`safety_get_diag()` (or GET
   `/api/status`) on the ESP for `diag_boot_reason` and the three decoded
   booleans, cross-check against the console banner and against the
   expected magic byte for that trigger.
4. Confirm the expected S6a trip on reset (link handshake), verify
   `trip_mask == 0x0020` only (reason 6, S6a/mainFault) before
   `safety_clear_trip()`; STOP and report if any other bit is set.
5. After all three triggers are captured, run `build_saftyfw()` in the
   **main tree** (confirming `git status`/HEAD first), then
   `debug_program(peer="pico", elf_path=<main tree's build/SaftyFW.elf>,
   confirm=true)` to restore unmodified HEAD, and verify via
   `safety_get_fw_version()` that the reported commit matches the main
   tree's `git rev-parse HEAD` at that moment and the dirty bit is clear.
6. Re-arm: confirm `commissioned=true`, S1 armed, S8 armed at
   20 C/min (`safety_set_rate_guard`/`safety_get_status`/`safety_get_diag`
   as appropriate) survived the flashes; if not, that is itself a
   persistence defect worth its own report per CLAUDE.md's standing
   guidance on this class of bug.
7. Confirm final state: relays off, nothing running, no trip latched.

## Not yet cleaned up

`C:\wt\saftydiag` (the worktree) and its four extra build directories are
left in place for the follow-up session to reuse the already-built ELFs
without rebuilding. Nothing in it is committed or referenced by the main
tree. Remove with `git worktree remove C:\wt\saftydiag --force` (from the
main tree) once the hardware pass above is complete and the ELFs are no
longer needed.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
