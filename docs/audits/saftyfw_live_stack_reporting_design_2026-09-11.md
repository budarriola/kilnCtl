# Design (not implemented): live per-task stack high-water marks from SaftyFW to the ESP

Scope: design only, per this pass's instructions. No code changed for this
section; `tools/run_all_checks.ps1`/`firmware/SaftyFW/test/
check_saftyfw_task_stack_budgets.py` remain the static, ELF-time picture
(see the companion 2026-09-11 measurement audit,
`docs/audits/saftyfw_bare_minimum_stack_measurement_2026-09-11.md`, and that
checker itself, now with a full ratcheted-baseline stack-margin check for
all 9 SaftyFW tasks). This document is the follow-on: how a *live* number,
read off the running board rather than derived from the ELF, could reach
the ESP and the existing tooling.

## Why this is wanted

The static checker's own numbers are explicitly LOWER BOUNDS — six of nine
tasks are flagged `INDETERMINATE` because their call graphs contain a
register-computed stack adjust (`add/sub sp, rN`) this walk cannot resolve
to an exact byte count, and none of it accounts for real scheduler/ISR
jitter beyond a flat 32 B allowance. `get_stack_margin()` on the KilnFW/ESP
side already closes exactly this gap for 29 ESP32-S3 tasks by reading
`uxTaskGetStackHighWaterMark()` live; SaftyFW has zero live coverage today.
`docs/audits/pico_reboots_at_heat_start_investigation_2026-09-11.md`
concluded two unexplained Pico reboots were most likely a `link_task`
overflow — unprovable precisely because no live number existed at the time.

## 1. `uxTaskGetStackHighWaterMark()` cost and calling context

- Cost: it walks the task's stack region from one end looking for the first
  byte that no longer matches FreeRTOS's fill pattern (`tskSTACK_FILL_BYTE`,
  0xA5 on this port) — O(stack depth in words) per call, no allocation, no
  blocking, safe to call from normal task context (it is a standard
  FreeRTOS API, not an ISR-restricted one). It reads memory only; it does
  not touch the target task's live registers or require that task to be
  suspended.
- It CANNOT safely be called from an ISR (no `...FromISR` variant exists in
  this FreeRTOS version) — ruling out reading it from `watchdog_task`'s own
  interrupt-adjacent paths or any hook run with interrupts disabled.
- It CAN be called by one task about another task's stack (pass the
  target's `TaskHandle_t`), so a single low-priority task (a good fit:
  `log_task`, already the lowest-priority, highest-margin task measured
  here at 472 B of 2048 B) could poll every other task's high-water mark
  from outside their own execution, rather than each task instrumenting
  itself. This avoids repeating the exact per-checkin, per-task
  instrumentation shape that was removed from `watchdog_task.c:290` (see
  item 3) — a single external poller, run at a slow, fixed, low-priority
  cadence, is a structurally different (and cheaper) load pattern than
  "every task pays this cost on every one of its own check-ins."
- Cost is still not free: a full walk of, say, `link_task`'s 10240 B/2560-word
  stack is meaningfully larger than the sub-256-word tasks. Any implementation
  should stagger which task is sampled per poll cycle (one task per tick of
  the poller, round-robin) rather than walking all nine every cycle, to keep
  any single poll cheap and bound worst-case jitter contribution.

## 2. Which link frame could carry it

- **DIAG (Frame B, `SAFETY_CMD_DIAG` / `KILNLINK_DIAG_CMD = 0x08`,
  `firmware/CommonFW/include/kilnlink/kilnlink_diag.h`) is fixed-size —
  `KILNLINK_DIAG_LEN = 26` bytes, exactly as many bytes as it already uses.
  Its header comment states the precedent for extending it without a
  version bump: reusing a *spare bit* in an already-transmitted byte (e.g.
  `boot_reason`'s bits 3-5 for `KILNLINK_DIAG_BOOT_STACK_OVERFLOW`/
  `_MALLOC_FAILED`/`_ASSERT_FAILED`, added 2026-09-09) is additive and does
  NOT require a `KILNLINK_PROTOCOL_VERSION` bump, because an older peer
  that has never heard of the new bit simply ignores it and the frame length
  does not change.
- A per-task high-water-mark report does not fit that precedent. Even the
  cheapest useful encoding — one byte per task, e.g. free-words-of-256
  clamped to 0-255, or a coarser quantized bucket — needs **9 new bytes**
  (one per task in `TASKS` today, and growing whenever a task is added,
  which is exactly the kind of count this repo already tracks mechanically
  via `check_saftyfw_task_count.py`). There is no spare-bit budget inside
  DIAG's existing 26 bytes for that; it would have to grow the frame's
  fixed length, which is a real wire-format change, not a same-length bit
  reuse.
- **This would require a `KILNLINK_PROTOCOL_VERSION` bump (currently 12,
  `firmware/CommonFW/include/kilnlink/kilnlink_version.h:235`) or a new
  frame command ID entirely** — say plainly, per this task's instructions,
  that this is a significant decision, not a detail. `docs/LINK_PROTOCOL.md`
  documents that a `KILNLINK_PROTOCOL_VERSION` bump is the mechanism this
  project uses specifically when "a peer would break" — i.e. an older
  firmware on one side of the link would misparse the new frame shape.
  Two credible options, both requiring that same review weight:
    - **(a) Grow DIAG itself** to a new fixed length (e.g. 26 + 9 = 35
      bytes) gated behind a version bump, so a peer below the new version
      keeps requesting/decoding the old 26-byte shape and only a
      version-negotiated peer sends/expects the longer frame. This follows
      the length-negotiation shape `docs/LINK_PROTOCOL.md` already
      describes for other version-gated growth (e.g. the sec 747 "skew
      safety" discussion of why some additions were NOT gated — the
      distinguishing factor there was always "does the frame's byte length
      change", and here it does).
    - **(b) A new, separate frame command** (its own `SAFETY_CMD_*` id,
      analogous to how `SAFETY_CMD_GET_CT_CAL`/`0x23` got its own id "since
      KILNLINK_PROTOCOL_VERSION 7" rather than growing an existing frame)
      carrying just the stack-margin payload, polled on its own slower
      cadence (this data changes on the order of minutes/boots, not the
      ~1 Hz DIAG already runs at) — probably the better fit precisely
      *because* it need not share DIAG's fast cadence, and a slow, separate,
      optional poll is easier to keep off any latency-sensitive path.
- Either path is a `KILNLINK_PROTOCOL_VERSION` bump. It does not have to
  move the **UART link version** (currently 11, distinct from and
  independent of the KILNLINK application-frame version — see project
  memory "Two Protocol Versions": `get_fw_version` reports the UART link's
  11, `/api/status` reports kilnlink's 12, both correct today) — the UART
  transport framing itself (start/length/CRC) is unaffected by adding a new
  application-level command or growing one frame's payload, so that number
  should stay at 11 unless this work also touches the transport layer
  itself, which it should not need to.

## 3. Timing risk — the removed 2026-08-23 instrumentation

`watchdog_task.c:290`'s comment records that a per-check-in
`uxTaskGetStackHighWaterMark(NULL)` diagnostic was added 2026-08-23,
answered its question (no task was near overflow at the time; the
tightest was `current_task` at 71 words free of 256), and was **removed**
because walking the unused-pattern fill on every check-in, from every task,
distorted the exact timing this code exists to measure: watchdog reset
cadence dropped from ~9 s to ~1.1 s. This is a documented, real, already-
observed failure mode for this exact primitive on this exact processor —
not a theoretical risk.

Implications for a real design:
- **Never call it from inside `watchdog_task_checkin()` or any other path
  the watchdog's own deadline logic depends on.** That is precisely the
  call site that was removed and precisely why.
- **Never call it from every task on every one of its own iterations.**
  The removed version's actual defect was frequency × breadth (9 tasks ×
  every check-in), not the primitive itself in isolation — a single call
  from a single low-priority poller task, at a slow fixed cadence (seconds,
  not per-tick), sampling one target task per cycle (see item 1's staggering
  point), is a fundamentally smaller and more bounded cost than "every task,
  every check-in."
- **Measure the jitter this new design adds, on real hardware, before
  trusting it**, the same way the 2026-08-23 removal was itself informed by
  a real measurement (9 s -> 1.1 s), not a guess. A reasonable acceptance
  gate: confirm watchdog reset cadence is unaffected (still ~9 s, not
  degraded) with the new poller running, before this is considered safe to
  ship — this project has exactly one prior data point for how badly this
  primitive can behave here, and it is bad enough that the fix was
  wholesale removal, not throttling.
- A slow, single-poller design is the right shape specifically *because* it
  sidesteps the frequency term that broke the 2026-08-23 attempt, but that
  is a prediction, not a measurement, until it is built and profiled.

## 4. A live high-water mark is a floor, not a worst case

This repo has an explicit, already-shipped finding to exactly this effect:
`docs/audits/saftyfw_bare_minimum_stack_measurement_2026-09-11.md`'s
"Verdict" section notes the one historical live number that does exist
(the removed 2026-08-23 measurement, `current_task` at 71 words free) was
taken from an **idle board** — no firing, relays off — and states plainly
that this is a floor, not a worst case, precisely because it never
exercised the task's actual worst-case call path (for `current_task`, the
2026-09-11 audit's own static analysis found no path anywhere near that
tight; the two paths that DID actually overflow historically,
`current_task`/`discrete_task` on bare `configMINIMAL_STACK_SIZE`, did so
under conditions — a dual reflash, specific core-1 timing — that an
idle-board sample would not have captured). Project memory records the same
lesson independently for a different subsystem (`project_idle_stack_
baseline_is_a_floor.md`: "captured with no firing, both LOW tasks have deep
paths that never ran").

A live reporting mechanism, once built, inherits this exactly: whatever
number ships to the ESP is only ever a high-water mark *of the conditions
that have actually occurred since boot*. A board that has been idle, or has
only ever taken shallow branches, will under-report every task's true worst
case — it can never over-report (the mark only grows, never shrinks, until
reboot), but it can silently miss the deep branch that has not fired yet.
This means:
- A live number complements the static checker; it does not replace it.
  The static checker's `INDETERMINATE`/lower-bound tasks stay the thing
  that bounds what the WORST case could be; a live number is evidence about
  what has ACTUALLY happened on a specific board's specific history, useful
  for confirming a specific incident or trend, not for clearing a task as
  categorically safe.
- Any surfaced value (in `get_heap_status`-style tooling, or a new
  `get_saftyfw_stack_margin()`) must be labeled with this caveat explicitly
  — a clean-looking live number after days of uneventful operation reads,
  to anyone unfamiliar with this history, as proof of safety it cannot
  actually provide. `get_stack_margin()`'s own KilnFW-side reporting should
  be checked for whether it already carries an equivalent disclaimer before
  a SaftyFW counterpart is built, so the same caveat is not needed twice in
  two different words.

## Summary of open decisions for a future implementation pass

1. DIAG-grow-with-version-gate vs. new-command-on-its-own-cadence — leaning
   toward the new command, since the data's natural update rate does not
   match DIAG's.
2. `KILNLINK_PROTOCOL_VERSION` bump to 13 either way — a decision for
   whoever implements this, not something to make unilaterally in a design
   pass; UART link version (11) is unaffected.
3. Single low-priority round-robin poller task (candidate: `log_task`,
   already lowest-priority with the most static headroom measured here),
   never per-task self-instrumentation on a hot check-in path.
4. A hardware timing measurement (watchdog cadence, unchanged) is a
   precondition for shipping this, not a nice-to-have — this primitive has
   a documented history of distorting exactly that measurement on this
   processor.
5. Every surfaced live number must carry the floor-not-ceiling caveat in
   its own output, not just in this document.
