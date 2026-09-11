# SaftyFW bare-minimum stack measurement — relay_owner / watchdog_task, 2026-09-11

Scope: measure, not guess, the worst-case stack depth of the two RP2040
(SaftyFW) tasks still running on bare `configMINIMAL_STACK_SIZE` (256 words,
`FreeRTOSConfig.h:60`) with no margin multiplier — `relay_owner` and
`watchdog_task` — flagged unmeasured by
`docs/audits/pico_fault_hook_diagnostics_audit_2026-09-11.md` (`8267fab2`).
No flashing, no heating run; read-only over the kilnctrl MCP where hardware
was consulted, everything else static analysis and host-side compiles.

## Why this matters

`docs/audits/pico_reboots_at_heat_start_investigation_2026-09-11.md`
(`26505ce6`) concluded the two unexplained Pico reboots at heat start were
most likely a `link_task` stack overflow, and `link_task` was on the
pre-raise bare-minimum stack at the time (it has since been raised — it is
not one of the two tasks in scope here). `link_task.c`'s own header
documents an earlier identical overflow on that task, and three SaftyFW
tasks have overflowed from bare-minimum stacks historically (project
memory: "SaftyFW minimal-stack overflows", "The brick was a stack
overflow"). `relay_owner` is the sole GPIO6 (relay) writer in the build and
the highest-priority task in the system — an overflow there is a safety
concern, not a cosmetic one.

## Method

`xTaskCreate` on this port takes stack depth in **words** — 256 words =
1024 bytes on this Cortex-M0+/32-bit target. Confirmed both tasks are
created with `configMINIMAL_STACK_SIZE` directly (`relay_owner.c:37,228`,
`watchdog_task.c:43,265`), not a `*_STACK_WORDS` macro sized off it.

1. Read `relay_owner_task()` and `watchdog_task_fn()` and every function they
   call transitively (`relay_grace.c`, `watchdog_gate.c`,
   `watchdog_overdue_diag.c`, `hal_gpio_pico.c`, `hal_wdt_pico.c`,
   `hal_scratch_pico.c`, plus the FreeRTOS `queue.c`/`tasks.c`/`list.c`
   entry points each task's loop reaches: `xQueueReceive`, `xTaskDelayUntil`,
   `xTaskGetTickCount`, `taskENTER_CRITICAL`/`EXIT_CRITICAL` (inline PRIMASK
   asm, no frame)).
2. Extracted the real compile command for `relay_owner.c.obj` from the
   existing `build.ninja` (`ninja -t commands`) and re-ran it, and the
   equivalent command for the other files above, with `-fstack-usage` added
   (`arm-none-eabi-gcc 14.2`, same flags: `-mcpu=cortex-m0plus -mthumb -O3
   -DNDEBUG`, i.e. the real Release flags this target ships with). This
   emits an exact per-function stack-frame size from the compiler itself,
   not a hand count — output kept in the session scratchpad, not committed
   (it is a byproduct of measurement, not a build artifact).
3. Summed frames along the deepest call path actually reachable from each
   task's loop body (not merely the largest single frame in each file).

## Measured per-function frame sizes (bytes, `-fstack-usage`, Release flags)

| Function | Frame (B) |
|---|---|
| `relay_owner_task` | 40 |
| `watchdog_task_fn` | 144 |
| `watchdog_task_checkin` | 8 |
| `watchdog_gate_all_within_deadline` | 20 |
| `xQueueReceive` (FreeRTOS `queue.c`) | 48 |
| `xQueueGenericSend` | 48 |
| `vTaskPlaceOnEventList` (`tasks.c`) | 32 |
| `xTaskResumeAll.part.0` | 24 |
| `xTaskCheckForTimeOut` | 24 |
| `xTaskDelayUntil` | 24 |
| `prvAddCurrentTaskToDelayedList` | 32 |
| `vListInsert` (`list.c`) | 12 |
| `xTaskGetTickCount` | 0 (leaf) |
| `vTaskCoreAffinitySet` | 24 |
| `xTaskCreate` | 48 |
| `relay_trip_transition` / `relay_clear_trip_transition` / `relay_grace_tick` | 0 (leaves, pure) |

`relay_grace.c` and `watchdog_gate.c` have no locals larger than a few
scalars and make no further calls (confirmed by reading — pure
state-transition arithmetic, the same property that makes them
host-testable). `hal_gpio_pico.c`/`hal_wdt_pico.c`/`hal_scratch_pico.c` are
thin one-line MMIO wrappers (`gpio_put`, `watchdog_update`, scratch-register
read/write) with no arrays, no `printf`/`vsnprintf`, and no loops — by
inspection, not `-fstack-usage`'d (an include-path issue in the ad hoc
recompile wasn't worth chasing further given how small these obviously are);
budgeted at 16 B each below, generously.

No large stack locals (arrays, frame buffers, formatting buffers) exist
anywhere in either task's reachable call graph — `watchdog_task_fn`'s own
144 B frame is the standout, and it is exactly the two fixed-size arrays it
declares itself: `TickType_t last_checkin[8]` (32 B) and
`watchdog_gate_entry_t entries[8]` (8 × 8 B = 64 B), `WATCHDOG_CHECKIN_COUNT`
being 8 (`watchdog_task.h`). No `vsnprintf`/formatting call exists in either
task.

## Worst-case call path per task

**`relay_owner_task`** — deepest reachable path is the queue-receive/
block-on-empty-queue path:

```
relay_owner_task (40) -> xQueueReceive (48) -> vTaskPlaceOnEventList (32) -> vListInsert (12)
= 40 + 48 + 32 + 12 = 132 B = 33 words
```

The command-handling branches (`hal_gpio_set`, `relay_trip_transition`,
`relay_clear_trip_transition`, `relay_grace_tick`, `watchdog_task_checkin`)
are all shallower than this (largest: `watchdog_task_checkin` at
40 + 8 = 48 B).

Adding a flat +32 B (8 words) for one level of Cortex-M0 hardware exception
entry stacking (the 8-register automatic push if an interrupt lands on this
task's own PSP mid-call — this port runs PendSV/SysTick off MSP, but the
*first* exception entry while a task is executing still stacks onto whatever
SP was active) as an explicit, disclosed margin:

```
132 + 32 = 164 B = 41 words, of 256 available -> 215 words / 84% free
```

**`watchdog_task_fn`** — deepest reachable path is the periodic-delay path:

```
watchdog_task_fn (144) -> xTaskDelayUntil (24) -> prvAddCurrentTaskToDelayedList (32) -> vListInsert (12)
= 144 + 24 + 32 + 12 = 212 B = 53 words
```

The gate-evaluation and diagnostic-latch branches
(`watchdog_gate_all_within_deadline`, `watchdog_overdue_diag_mark`,
`watchdog_overdue_diag_notify_recovered`, `hal_wdt_feed`, `hal_gpio_set`) are
all shallower (largest estimated: 144 + ~16 (diag-mark's own frame,
by-inspection budget) + 16 (`hal_scratch_read/write_u32`, by-inspection
budget) ≈ 176 B).

With the same +32 B exception-entry margin:

```
212 + 32 = 244 B = 61 words, of 256 available -> 195 words / 76% free
```

## This is a LOWER BOUND, and why

Per this repo's existing convention for the ESP-side stack checkers,
treat both totals above as a floor, not a ceiling:

- FreeRTOS's `queue.c`/`tasks.c` call graphs were traced only along the one
  path each task's loop body actually reaches (confirmed by reading the
  loop bodies), not exhaustively for every function `-fstack-usage` listed
  in those two files — a genuinely indirect/function-pointer call inside
  FreeRTOS itself (there are a few, e.g. via `pxCurrentTCB`) was not
  hand-verified to be excluded from these two paths.
- Multiple nested interrupts landing back-to-back on the same task's stack
  before it unwinds would each add their own ~32 B, not accounted for beyond
  the one flat margin above.
- The three HAL backends (`hal_gpio_pico.c`, `hal_wdt_pico.c`,
  `hal_scratch_pico.c`) were budgeted at 16 B each by inspection rather than
  compiled with `-fstack-usage` (an `-I` path issue in the throwaway command
  wasn't worth debugging further given they are one-line MMIO wrappers with
  no locals of consequence) — if wrong, this is very unlikely to be wrong by
  more than a few tens of bytes.

Even so, both totals land far short of half the 256-word budget, and the
standing counter-example in the same file (`watchdog_task.c:290`, historical
2026-08-23 measurement) shows `current_task` — a genuinely SPI/ADC-heavy
task — using 185 of 256 words (71 free) on bare minimum stack, i.e. the
minimum stack size itself is not inherently too small for a task this
simple; `current_task`'s tightness comes from its SPI/ADC call depth, which
neither `relay_owner` nor `watchdog_task` shares.

## Live high-water marks: not obtainable without flashing

- `kiln_find(query="stack high water mark safety pico")` surfaces exactly
  one relevant tool, `get_stack_margin()` — checked live via
  `kiln_call(name="get_stack_margin", args={})`. It reports 29 **KilnFW
  (ESP32-S3)** tasks (`httpd_worker`, `safety_poll`, `link_watchdog`, etc.)
  and nothing from SaftyFW. This is the ESP-side `stack_margin.c` mechanism
  (`firmware/KilnFW/App/drivers/common/stack_margin.c`) — it has no RP2040
  counterpart today.
- No DIAG-frame field or other link-protocol path currently carries
  RP2040-side `uxTaskGetStackHighWaterMark()` values to the ESP or to any
  MCP tool. The only historical live number for either task in scope comes
  from a comment left in the source, not a currently-running mechanism:
  `watchdog_task.c:290` records that a **since-removed** 2026-08-23
  per-check-in instrumentation pass measured `current_task` (not one of the
  two tasks in scope) at 71 words free of 256 — removed because walking the
  unused-pattern fill on every check-in from every task added enough jitter
  by itself to distort the watchdog cadence it was measuring (uptime between
  resets dropped from ~9 s to ~1.1 s). No equivalent measurement was ever
  taken for `relay_owner` or `watchdog_task` specifically, live or
  otherwise, before this pass's static analysis above.
- Getting a real number would require either re-adding that same disruptive
  instrumentation temporarily (rejected for the reason it was removed) or a
  new low-jitter mechanism (e.g. one lazy high-water-mark read per task per
  boot rather than per check-in) — a reasonable follow-up, not attempted
  here since it requires a flash to observe, which this task is scoped
  never to do.

## Verdict: no raise needed, none made

Both `relay_owner` (measured/derived worst case ≈ 41 words including
margin) and `watchdog_task` (≈ 61 words including margin) have wide,
measured headroom against their shared 256-word budget — even generously
padded for the unresolved uncertainty in the FreeRTOS/HAL call graph noted
above, neither approaches half the budget, let alone the danger zone that
actually bit `link_task` and the other two historical incidents. No stack
depth was changed. Per this repo's standing rule (`feedback_negative_test_
every_check.md`'s spirit, applied here: don't raise "to be safe" without a
measured basis), raising either constant now would not be justified by
anything found in this pass. `link_task` is not addressed here (out of
scope — the audit that flagged this task explicitly named only
`relay_owner`/`watchdog_task` as the still-unmeasured pair; `link_task` was
already raised per that audit's own framing, "every other task uses a
raised `*_STACK_WORDS` macro").

No code was changed, so the build/host-test/`run_all_checks.ps1`
re-verification steps that would apply to an actual stack-depth change were
not run — nothing to verify. The `-fstack-usage` recompiles were done
outside the tracked build tree, against throwaway object files in the
session scratchpad; the repo's `build.ninja` and CMake caches were not
touched.

## Mechanical stack-margin check: gap confirmed

`tools/check_stack_margin_registration.ps1` and
`tools/check_stack_margin_baseline.ps1` exist and are **KilnFW/ESP-only** —
both operate against `firmware/KilnFW/App/drivers/common/stack_margin.c`
and its task-registration list. No equivalent check exists anywhere under
`firmware/SaftyFW/` or the top-level `tools/` for the RP2040 side (searched
for `*stack_margin*` and `check_stack*` repo-wide, excluding worktrees and
build directories — nothing SaftyFW-shaped came back). This means:

- SaftyFW's task-to-stack-depth mapping (which tasks use bare
  `configMINIMAL_STACK_SIZE` vs. a raised `*_STACK_WORDS` macro) is enforced
  by nothing mechanical — it is exactly the kind of fact that drifted
  quietly enough for this pair to go unmeasured until an audit happened to
  name it by hand.
- There is also no live high-water-mark reporting for SaftyFW tasks at all
  (see above), so even a registration check modeled on the ESP one would
  have nothing to compare a baseline against without first building that
  reporting path.

**Finding, not fixed in this pass**: a SaftyFW-side stack-margin
registration check (mirroring `check_stack_margin_registration.ps1`'s
intent: every `xTaskCreate` call site must use a named `*_STACK_WORDS`
constant, not a bare `configMINIMAL_STACK_SIZE` literal, so a reviewer
sees the choice made explicitly rather than by omission) is a reasonable
follow-up. Out of scope here because building and negative-testing a new
mechanical check is a separate, larger unit of work than the measurement
this task asked for, and the two current bare-minimum users were just shown
to have real headroom, not an active defect that check would have caught.
