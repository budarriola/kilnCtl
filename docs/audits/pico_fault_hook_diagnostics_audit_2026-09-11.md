# Pico (SaftyFW) fault-hook diagnostics audit — 2026-09-11

Scope: FreeRTOS malloc-failure / stack-overflow / `configASSERT` fault hooks,
the `boot_reason` producer/consumer chain, and RP2040 task stack sizing.
Analysis and test design only — no flashing, no heating run, nothing executed
on hardware. This addresses the "consumer without producer" and "reset one
side of a pair" bug classes documented in `CLAUDE.md`, applied to a path that
has never actually been verified on the bench.

## 1. Producer/consumer file:line map

### FreeRTOS fault hooks (producers)

| Hook | Location | Action on fire |
|---|---|---|
| `configASSERT(x)` | `firmware/SaftyFW/FreeRTOSConfig.h:238-244` | Writes `WATCHDOG_FATAL_ASSERT_WORD(file_id, __LINE__)` (tag `0xA5`) to `watchdog_hw->scratch[5]`, disables interrupts, spins forever. Deliberately a raw MMIO macro (no function call) so it is safe from an ISR or a corrupted-stack context — see the header comment at `FreeRTOSConfig.h:131-136` and `watchdog_overdue_diag_codec.h`'s packing-macro comment. |
| `vApplicationStackOverflowHook()` | `firmware/SaftyFW/src/main.c:69-163` | Line 158: writes tag `0xE3` plus the first two bytes of the failing task's name to `scratch[5]`; lines 160-162 disable interrupts and spin. |
| `vApplicationMallocFailedHook()` | `firmware/SaftyFW/src/main.c:165-193` | Line 188: writes `WATCHDOG_FATAL_MALLOC_WORD()` (tag `0xB4`) to `scratch[5]`; lines 190-192 disable interrupts and spin. |

All three share one physical register, `watchdog_hw->scratch[5]`, distinguished
by tag byte; this is the *same* register used by the older overdue-checkin
latch (tag `0xD9`), by design — "at most one of the two will ever report
`magic_ok == true` for a given boot" (`main.c:390`). `configCHECK_FOR_STACK_OVERFLOW`
= 2 and `configUSE_MALLOC_FAILED_HOOK` = 1 are turned on unconditionally
(`FreeRTOSConfig.h:121,124`).

None of the three actually resets the board — they halt with interrupts
disabled and let the **hardware watchdog** (fed by `watchdog_task`, separate
mechanism) time out and reset. This means the value only survives to the next
boot if the watchdog is actually running and armed; a hook firing before
`watchdog_enable()` runs (very early boot) would spin forever with no reset
at all. Not flagged as a defect — see the RP2040 SDK's watchdog-scratch-word
persistence assumption already documented in `boot_reason.h`'s header comment
— but worth confirming during the hardware verification pass below.

### `watchdog_fatal_diag_t` — decode/read/cache (consumer, Pico side)

- Struct and tag enum: `firmware/SaftyFW/src/watchdog_overdue_diag_codec.h:99-113`
  (`WATCHDOG_FATAL_KIND_NONE=0, STACK_OVERFLOW=1(0xE3), MALLOC_FAILED=2(0xB4), ASSERT=3(0xA5)`).
- Read-before-clear, boot step 3c: `firmware/SaftyFW/src/main.c:396` —
  `watchdog_fatal_diag_t watchdog_fatal_diag = watchdog_fatal_diag_read();`
  — read *after* `watchdog_overdue_diag_read()`/`watchdog_overflow_diag_read()`
  (order between the three reads doesn't matter, comment at `main.c:392-395`
  says so explicitly — they all decode the same still-uncleared word) but
  strictly *before* the shared clear at `main.c:443`
  (`watchdog_overdue_diag_clear(); // clears the one shared register regardless of which format was present`).
- Console report: `main.c:422-436` — human-readable text on the debug-probe
  UART, reachable with a plain serial terminal and no JTAG session.
- Host-level unit coverage of the pure decode logic already exists:
  `firmware/SaftyFW/test/test_watchdog_overdue_diag_codec.c` — test name
  `watchdog_fatal_diag_decode -- the unified stack-overflow/malloc-fail/
  assert latch, 2026-09-09 RP2040 fatal-fault diagnosability pass` (seen in
  the host-test run below). This tests the codec's bit math, not the ARM
  Cortex-M0+ hooks that write the raw word — the hooks themselves cannot run
  under a host build.

### Wire encoding, Pico → ESP

- Bit definitions (source of truth): `firmware/CommonFW/include/kilnlink/kilnlink_diag.h:47-52`
  ```
  KILNLINK_DIAG_BOOT_POWERON        = 0x01
  KILNLINK_DIAG_BOOT_WATCHDOG       = 0x02
  KILNLINK_DIAG_BOOT_BROWNOUT       = 0x04  (unused — no detector, always 0)
  KILNLINK_DIAG_BOOT_STACK_OVERFLOW = 0x08  (line 50)
  KILNLINK_DIAG_BOOT_MALLOC_FAILED  = 0x10  (line 51)
  KILNLINK_DIAG_BOOT_ASSERT_FAILED  = 0x20  (line 52)
  ```
- Assembly into the DIAG frame's `boot_reason` byte:
  `firmware/SaftyFW/src/tasks/link_task.c:984-1024`. Reads the fatal latch
  from the **RAM cache**, not the register, deliberately:
  `watchdog_fatal_diag_t fatal = watchdog_fatal_diag_get_cached();`
  (line ~1007) — because `link_task_send_diag()` runs repeatedly for the life
  of the boot while the physical register was read once and cleared at boot
  step 3c. Same convention as `boot_reason_get_cached()` immediately above it
  in the same function.
- Packed into `kilnlink_diag_t.boot_reason` and sent via the shared
  `kilnlink_diag_encode()` codec (`firmware/CommonFW/src/kilnlink_diag.c`),
  byte offset 10 of the DIAG frame (`kilnlink_diag.h:24,88`; wire layout also
  documented in `firmware/CommonFW/docs/LINK_PROTOCOL.md`, Frame B table).

### ESP-side cache and exposure (consumer)

- Frame parse / cache write, unconditional on every DIAG frame:
  `firmware/KilnFW/App/drivers/safety/safety_link_frames.c:887` —
  `link->cached.diag_boot_reason = p[10];`
- Hand-mirrored bit constants (mykicadMcp-style duplication, since
  `kilnlink_diag.c` is not compiled into the ESP-IDF component — see that
  file's own header comment): `firmware/KilnFW/App/drivers/safety/safety_link.h:285-297`,
  including the three fatal bits at lines 295-297.
- Further exposure to payload/HTTP: `firmware/KilnFW/App/drivers/safety/safety_link_payload.c:188`
  (`out[11] = status.diag_boot_reason;`), `dashboard_http.c:315` /
  `dashboard_http.h:287-293`, and the JSON API surface
  `firmware/KilnFW/App/drivers/http/dashboard_status_http.c:498-512`, which
  decodes `diag_boot_reason` into `stack_overflow`/`malloc_failed`/`assert_failed`
  booleans on `GET /api/status`.

## 2. Defect found: mirror-drift check did not cover the three new bits

**Severity: medium (latent — a silent-divergence risk, not an active bug
today).**

`firmware/KilnFW/App/test/power_diag_flag_mirror_drift_check.py` exists
*specifically* to catch the hand-mirrored-constant class documented in its own
header ("A silent divergence between the mirror and the source of truth
passes compilation on both sides and misdecodes flags/lengths at runtime").
Its `MIRROR_MAP` covered `KILNLINK_DIAG_BOOT_POWERON` / `_WATCHDOG` / `_BROWNOUT`
but, before this pass, **not** the three fatal-fault bits
(`_STACK_OVERFLOW` / `_MALLOC_FAILED` / `_ASSERT_FAILED`) added 2026-09-09
alongside the fatal-diag feature itself (`kilnlink_diag.h:50-52`,
`safety_link.h:295-297`). Both sides currently agree (`0x08`/`0x10`/`0x20` on
both), so this was not yet an active misdecode — but the one check built to
prevent exactly this class of defect had a hole exactly where the newest,
least-tested diagnostic bits live: the ESP's decoded `stack_overflow` /
`malloc_failed` / `assert_failed` booleans could have silently gone stale on
the next edit to either side with nothing catching it until read on a real
fault, the worst possible time to discover a decode bug.

**Fix applied** (one-line-shaped, mechanically identical to the 21 existing
entries in the same list): added the three missing tuples to `MIRROR_MAP` in
`firmware/KilnFW/App/test/power_diag_flag_mirror_drift_check.py`.

**Negative-tested by hand**: temporarily changed
`SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED` in `safety_link.h` from `0x20u` to
`0x40u`, re-ran the check, confirmed it failed with:
```
safety_link.h:SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED = 64 but kilnlink_diag.h:KILNLINK_DIAG_BOOT_ASSERT_FAILED = 32
```
then restored the line by hand (not via `git checkout`) and confirmed
`git diff firmware/KilnFW/App/drivers/safety/safety_link.h` was empty before
re-running the check clean:
`POWER/DIAG FLAG MIRROR DRIFT CHECK: OK (23 mirrored constants agree)`.

## 3. Producer-existence assessment per bit

| Bit | Producer exists? | Notes |
|---|---|---|
| `BOOT_POWERON` (0x01) | Real, but only as "not a watchdog reset" (`link_task.c` comment: no true power-on-reset detector) | Not a fault-hook diagnostic; included for completeness |
| `BOOT_WATCHDOG` (0x02) | Real — `watchdog_caused_reboot()` (pico-sdk), read at `main.c` boot step 3 | Live today, already routinely exercised (any watchdog-triggered reset sets it) |
| `BOOT_BROWNOUT` (0x04) | **Dead — no detector built** | Documented as such in the source; always 0. Pre-existing known gap, not new. |
| `BOOT_STACK_OVERFLOW` (0x08) | Real producer exists (`vApplicationStackOverflowHook`) and this codebase has tripped it three times historically (project memory: "SaftyFW minimal-stack overflows", "The brick was a stack overflow") | **Never confirmed to survive the full round trip to the ESP's decoded API field on real hardware** — the historical incidents were diagnosed via JTAG/SWD reads of `s_bg`/scratch state directly, not via this DIAG-frame path, per project memory (`project_safety_poll_panic_thermo_slot_corruption.md` uses this pattern for the *ESP* side; no SaftyFW-side note confirms the RP2040 path end-to-end) |
| `BOOT_MALLOC_FAILED` (0x10) | Real producer exists (`vApplicationMallocFailedHook`); no confirmed historical trip on this board | Never observed to fire on real hardware; heap use on SaftyFW is deliberately minimal, so this may be genuinely rare-to-never in practice, but the mechanism itself is untested |
| `BOOT_ASSERT_FAILED` (0x20) | Real producer exists (`configASSERT`); used pervasively throughout SaftyFW (RP2040) code, so plenty of trip sites exist in principle | Never confirmed to reach the ESP over the link on real hardware |

No dead producers were found among the fatal-fault trio — all three hooks are
real, wired, FreeRTOS-standard mechanisms, correctly enabled in
`FreeRTOSConfig.h`. The genuinely dead bit is the pre-existing `BOOT_BROWNOUT`,
which is documented as such in the source and is not part of this audit's
"never verified" trio.

## 4. "Reset one side of a pair" check

Walked the full latch/clear lifecycle on both processors:

- **Pico, fatal-hook latch (`scratch[5]`)**: single producer path (three
  hooks, one register), single read (`watchdog_fatal_diag_read()`,
  `main.c:396`), single clear (`watchdog_overdue_diag_clear()`, `main.c:443`),
  strictly read-before-clear, all within one boot's linear sequence before
  the scheduler starts. No second clear path exists on the Pico side.
- **Pico, `trip_reason` latch** (separate register pair, scratch[0]/[1]):
  set by `boot_reason_latch_trip()` (`boot_reason.c:51-55`, called from
  `safety_core.c` on a new guard trip), cleared once by
  `boot_reason_clear_trip()` (`boot_reason.c:57-60`, called once from
  `main.c` near line 369). Also single clear path.
- **ESP, `diag_boot_reason` cache**: **no explicit clear function exists.**
  `safety_link_frames.c:887` unconditionally overwrites the cached byte on
  *every* successfully parsed DIAG frame. This is a live mirror, not an
  independently-latched value — so there is nothing for a Pico-side clear to
  fall out of sync with. A Pico reboot that clears the physical register
  before the next DIAG frame is sent will correctly cause the very next DIAG
  frame's `boot_reason` byte to read the post-clear value (0, or whatever the
  *new* boot's own reads produce), and the ESP's cache follows one frame
  later.
- **Conclusion: this pair does *not* exhibit the "reset one side" bug class.**
  Unlike the four prior instances (PC-side `msg_index`, SimFW's `s_ring_next_seq`,
  `fault_sched.c`'s `s_seed`, and pre-fix `trip_last_seq`), the ESP side here
  has no independent state to desync — it is a pure read-through cache with
  exactly one producer of truth (the current DIAG frame). The class the doc
  warns about requires *two independently-initialized* pieces of state joined
  by an implicit contract; that shape isn't present in this particular pair.
  The actual defect found (section 2) is a different, narrower risk: a stale
  *symbol value* mirror, not a stale *runtime state* mirror — but it is the
  same "hand-duplicated pair, no shared type" root cause the reset-one-side
  writeup calls out as the structural pattern to watch for.

## 5. RP2040 task stack depths (words, not bytes)

`configMINIMAL_STACK_SIZE` = **256** (words) on target
(`firmware/SaftyFW/FreeRTOSConfig.h:60`; the host-stub build uses a different,
irrelevant value of 128 at `test/stubs/freertos_min/FreeRTOS.h:42`, host-only).

All 9 `xTaskCreate()` call sites pass a named `*_STACK_WORDS` macro, never a
bare literal — so a bytes/words mixup, if present, would live inside a macro
definition, not at the call site. Definitions found:

| Task | Macro | Value (words) | Definition |
|---|---|---|---|
| `current_task` | `CURRENT_TASK_STACK_WORDS` | `256 * 6` = 1536 | `current_task.c:40` |
| `discrete_task` | `DISCRETE_TASK_STACK_WORDS` | `256 * 4` = 1024 | `discrete_task.c:36` |
| `link_task` | `LINK_TASK_STACK_WORDS` | `256 * 10` = 2560 | `link_task.c:153` |
| `log_task` | `LOG_TASK_STACK_WORDS` | `256 * 2` = 512 | `log_task.c:77` |
| `relay_owner` | `RELAY_OWNER_STACK_WORDS` | `256` (bare `configMINIMAL_STACK_SIZE`) | `relay_owner.c:37` |
| `safety_core` | `SAFETY_CORE_STACK_WORDS` | `256 * 6` = 1536 | `safety_core.c:154` |
| `thermo_task` | `THERMO_TASK_STACK_WORDS` | `256 * 4` = 1024 | `thermo_task.c:54` |
| `update_task` | `UPDATE_TASK_STACK_WORDS` | `256 * 6` = 1536 | `update_task.c:125` |
| `watchdog_task` | `WATCHDOG_TASK_STACK_WORDS` | `256` (bare `configMINIMAL_STACK_SIZE`) | `watchdog_task.c:43` |

All values are already correctly expressed in **words** (the macro name says
so, and each multiplies a word-count constant by an integer) — no
bytes-for-words mixup found; this is not a repeat of the historical
`configMINIMAL_STACK_SIZE`-bare overflow bug. Two tasks, `relay_owner` and
`watchdog_task`, remain at the bare 256-word (1024-byte) minimum with no
multiplier. Per project memory ("SaftyFW minimal-stack overflows" — "three
RP2040 tasks on bare `configMINIMAL_STACK_SIZE`; one masqueraded as a dead
safety link"), this class has bitten this codebase before; `relay_owner` and
`watchdog_task` are two of the plausible remaining candidates from that
history, though whether they are the *same* two tasks flagged before, or
already-fixed successors, was not established in this pass — the repo's
`check_saftyfw_task_stack_budgets.py` (`firmware/SaftyFW/tools/`) exists to
catch stack-margin regressions mechanically and enumerates exactly these 9
tasks; not re-run in this pass since it requires an instrumented build, out
of scope for analysis-only work. **Recommendation, not a fix**: run that
budget tool (or capture stack high-water marks via
`uxTaskGetStackHighWaterMark()` on the bench board) for `relay_owner` and
`watchdog_task` specifically before relying on the fatal-hook path to catch a
future overflow in either — a hook that never gets the chance to fire because
the watchdog resets the board via a *different* mechanism first (e.g. a
missed check-in from a task that's already spinning past its stack) would
look, from the ESP's perspective, identical to "boot_reason bit correctly
reports nothing happened."

## 6. Host test result

Ran via Bash (per project memory, the PowerShell tool is spurious for this
script):
```
powershell -ExecutionPolicy Bypass -File firmware/SaftyFW/test/build_host_tests.ps1
```
Result: **all suites passed**, exit code 0, final line `all passed`. Notably
includes `test_watchdog_overdue_diag_codec.c`'s
`watchdog_fatal_diag_decode -- the unified stack-overflow/malloc-fail/assert
latch, 2026-09-09 RP2040 fatal-fault diagnosability pass` — this confirms the
*codec* (bit-packing/decoding logic) is unit-tested and correct at the host
level. It does **not** and cannot exercise the actual ARM hook bodies
(`vApplicationStackOverflowHook`, `vApplicationMallocFailedHook`,
`configASSERT`'s scratch-register write) or the physical RP2040 watchdog
scratch registers, since those depend on real hardware/pico-sdk. This is
exactly the gap this audit's hardware procedure (section 7) is meant to
close.

## 7. Hardware verification procedure (design only — NOT executed)

General safety notes for all three: run with the board disconnected from
mains/heater outputs or with relays confirmed de-energized beforehand
(`kiln_call(name="safety_get_status")` / relay state check); a later session
with flash authority is expected to add small, temporary, clearly-marked
test-only trigger code for each (a debug HTTP/serial command, or a build-time
`#ifdef`), never ship a permanent way to crash the safety processor on
demand. Do not attempt any of this in the current no-flash task.

### 7a. `configASSERT` (bit 0x20, `ASSERT_FAILED`)

- **Provoke**: add one temporary, obviously-dead-code `configASSERT(false)`
  behind a debug-only serial/HTTP command (e.g. a new `TEST_TRIGGER_ASSERT`
  command in the existing debug console, gated so it cannot be reached in a
  normal build) — pick an assert site with a real `SAFTYFW_ASSERT_FILE_ID`
  already declared for the containing file, so `file_id`/`line` decode
  meaningfully.
- **Observe**: after the subsequent watchdog-triggered reset, read the
  Pico's own debug-probe UART for the `"!!! last boot: configASSERT FAILED,
  file_id=%u line=%u\r\n"` line (`main.c:428-430`); separately, poll the ESP
  via `kiln_call(name="safety_get_status")` or the raw
  `GET /api/status`-family endpoint and confirm `assert_failed: true` with
  the correct decoded state, then confirm it reads `false` again after the
  *next* Pico boot (proving the register was actually cleared, not stuck).
- **Return to safe state**: remove the temporary trigger code before any
  further work; confirm via `safety_get_status()`/`get_heap_status()` that no
  trip/fault condition persists and relays remain de-energized; re-flash
  clean firmware if the temporary hook was compiled in rather than
  runtime-gated.

### 7b. `vApplicationStackOverflowHook` (bit 0x08, `STACK_OVERFLOW`)

- **Provoke**: temporarily reduce one low-risk task's stack macro (a
  logging-only task, not `safety_core`/`relay_owner`/`watchdog_task`) to an
  artificially tiny value, or add a debug-only deep-recursion trigger gated
  the same way as 7a, so it can be reverted with a one-line diff.
- **Observe**: same two-sided check as 7a — Pico UART's
  `"!!! last boot: STACK OVERFLOW, task '%c%c...'\r\n"` line (`main.c:414-418`)
  showing the correct first two characters of the overflowed task's name;
  ESP's `stack_overflow: true` on the next DIAG frame, then `false` on the
  following boot.
- **Return to safe state**: revert the stack-size reduction / remove the
  recursion trigger; run `check_saftyfw_task_stack_budgets.py` afterward to
  confirm no task is left under-provisioned; confirm no trip persists.

### 7c. `vApplicationMallocFailedHook` (bit 0x10, `MALLOC_FAILED`)

- **Provoke**: add a temporary, debug-gated loop that calls `pvPortMalloc()`
  in a tight loop without freeing until it returns NULL (heap exhaustion) —
  guarded the same way as 7a/7b, ideally behind the same single test-trigger
  command dispatched by an enum so only one of the three runs per boot.
- **Observe**: Pico UART's `"!!! last boot: MALLOC FAILED (heap
  exhausted)\r\n"` line (`main.c:420`); ESP's `malloc_failed: true` on the
  next DIAG frame, then `false` on the following boot.
- **Return to safe state**: remove the trigger loop; confirm heap headroom
  is back to baseline via `get_heap_status()`-equivalent on the Pico side (or
  the existing SaftyFW heap-diagnostic path if one is exposed) and that no
  trip persists.

### Common regression check for all three

Run 7a/7b/7c **in sequence on the same boot cycle** at least once (trigger
one, observe, reboot to clear, trigger the next) to positively demonstrate
that the shared `scratch[5]` register's tag-byte discrimination actually
works on hardware — i.e., that a MALLOC_FAILED boot is never misreported as
ASSERT_FAILED or vice versa. This is exactly the kind of cross-contamination
a shared-register design could hide, and the host-test codec coverage
(section 6) cannot catch it because the host build never touches the real
register.

## Summary

- **3 of 3** fatal-fault diagnostic bits (`STACK_OVERFLOW`, `MALLOC_FAILED`,
  `ASSERT_FAILED`) have real, correctly-wired producers on the RP2040 side;
  none are dead code. All three are **unverified on real hardware** — the
  producer→register→cache→link-frame→ESP-cache→HTTP-API chain has never been
  observed to complete end-to-end on the bench, only unit-tested at the codec
  level.
- `BOOT_BROWNOUT` (pre-existing, not in scope) remains a documented dead bit
  (no detector).
- **Defect found and fixed** (medium severity, one-line-shaped): the
  mirror-drift check (`power_diag_flag_mirror_drift_check.py`) was missing
  the three fatal-fault bit constants from its `MIRROR_MAP`, leaving the
  newest diagnostic bits uncovered by the exact check built to catch this
  class of silent divergence. Fixed, negative-tested (confirmed it fails on
  an injected mismatch, then restored by hand and confirmed clean), and
  re-verified passing.
- No "reset one side of a pair" defect found in this specific latch/cache
  pair — the ESP side is a pure read-through cache with no independent state
  to desync.
- RP2040 task stacks are correctly expressed in words, not bytes; no
  bytes/words mixup found. `relay_owner` and `watchdog_task` remain at the
  bare `configMINIMAL_STACK_SIZE` (256 words / 1024 bytes) with no safety
  margin multiplier — flagged for follow-up measurement, not fixed here
  (out of scope; requires the stack-budget tool or on-hardware high-water
  mark capture).
- SaftyFW host tests: **all passed**, exit 0, via
  `powershell -ExecutionPolicy Bypass -File firmware/SaftyFW/test/build_host_tests.ps1`
  run through the Bash tool.
- Hardware verification procedures for all three fatal-fault bits are
  designed above (section 7) but deliberately **not executed** — left for a
  later session with flash authority, per this task's scope.
