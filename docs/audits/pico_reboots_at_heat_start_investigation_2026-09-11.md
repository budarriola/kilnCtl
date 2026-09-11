# Pico reboots at heat start — forensic investigation (2026-09-11)

Purely archival/live-read investigation, no reflash, no heating run performed
in this session. Scope: the two unexplained RP2040 (SaftyFW) reboots recorded
in `docs/audits/cplval45_scale_vs_offset_aborted_2026-09-10.md` and
follow-up `docs/audits/pico_double_reboot_investigation_2026-09-10.md`
(commit `238e0eb9` and later). A separate, concurrent audit
(`docs/audits/pico_fault_hook_diagnostics_audit_2026-09-11.md`, if present)
covers the general fault-hook/`boot_reason` producer/consumer mapping in
`firmware/SaftyFW/`; that file did not exist at the time this document was
written, so no citation to it could be made — this document derives its own
`boot_reason` findings independently, scoped to these two specific reboots.

## 1. What the original evidence actually showed

From `docs/audits/cplval45_scale_vs_offset_aborted_2026-09-10.md`:

- Pre-flight: SaftyFW `boot_id=58`, `uptime≈818 s`, `trip_mask=0x0000`, link up,
  commissioned, no crash banner.
- `profiles_start(profile_id=7)` issued (ESP-side firing start; nothing in
  that session touched the Pico directly — no flash, no OTA, no
  `debug_reset`).
- ~30 s in, `safety_get_diag` showed `uptime 12006 ms` — the Pico had
  rebooted once. `boot reason: watchdog` both times per the log.
- The run was stopped; a follow-up check found a **second** reboot
  (`uptime 6006 ms`, `boot_id` 58→60 across the two events) within ~2 minutes
  of the first.
- `trip_mask=0x0000` throughout both events — **no safety trip ever latched**.
  This matters: it rules out a guard genuinely tripping and the trip path
  itself crashing (`safety_core.c`'s trip-latch code was never exercised, per
  the trip_mask reading).
- What was explicitly **not** established at the time: the actual crash
  mechanism. The document lists it as "Cause — not yet determined" and names
  three unranked candidates (SX1509 re-init timing, link-handshake retry
  storm, or a genuine watchdog defect) as things "not yet investigated."

From `docs/audits/pico_double_reboot_investigation_2026-09-10.md` (same-day
follow-up, before any fix was known to be the cause):

- Direct SWD read of the **still-running, pre-fix** build (`a87672ab`)
  confirmed `WATCHDOG->REASON = 0x00000001` (hardware-forced/watchdog reset,
  consistent with the log's "boot reason: watchdog" both times) but found
  `watchdog_hw->scratch[5] = 0x00000000` — **no fatal-fault tag** from any of
  the three diagnostic hooks (`vApplicationMallocFailedHook`'s `0xB4`,
  `configASSERT`'s `0xA5`, `stack_overflow_hook`'s `0xE3`).
- Critically, that document itself explains why a clean `scratch[5]` is
  **not exculpatory**: commit `1496e288` ("stop the fatal-fault diagnostic
  from erasing its own evidence") was not yet on the running build
  (`a87672ab` predates it), and without it `watchdog_task` running on the
  other core can overwrite a genuine fatal tag with its own `0xD9`
  "overdue checkin" tag before the reset lands — and the overdue-diag reader
  then clears a `0xD9`-tagged `scratch[5]` on the next boot as an ordinary,
  non-fatal event. So the forensic read that would have identified *which*
  hook fired (if any) was destroyed by the very bug this class of fix
  addresses, before this session's SWD probe could see it. This is an
  assumption gap, not a finding: **the original two reboots' direct
  cause was never captured and cannot be recovered retroactively.**
- After flashing SaftyFW to HEAD (`238e0eb9`) and re-running the same
  profile for 186+ seconds (past both the ~30 s and ~2 min marks of the
  original two reboots), **the Pico did not reboot** — `boot_id` held at 58
  throughout. The run ended on an unrelated ESP panic, not a Pico event.
- That document is explicit that one clean run is "not proof of a fix" and
  that "none of today's landed SaftyFW fixes can be named as *the* cause,"
  because the pre-flash forensic read came back empty for the reason
  explained above.

`docs/audits/cplval45_scale_vs_offset_2026-09-10.md` (the successful
re-attempt, commit range after `238e0eb9`) records a second data point: a
full ~46-minute run to a 45°C dwell, again with no Pico reboot (`boot_id=255`
held for the whole run, `uptime` climbing 296 s → 3676 s), and explicitly
attributes the non-recurrence to "the Pico was reflashed to current
firmware; three tasks that were on bare `configMINIMAL_STACK_SIZE` were
fixed; `link_task`/`update_task` stacks were raised with the FreeRTOS heap
raised to match." This is the same non-conclusive framing as the prior
document (a second clean run, not proof) but adds a second window of
non-recurrence after the same set of fixes.

## 2. What actually changed between the failing build and the fixed build

The build running at the time of both reboots was **`a87672ab`**
(2026-09-09 17:53:58 -0700, per `git log`). The two audit documents above
report this exact hash. Comparing it against the fixes named as the likely
cause:

| fix | commit | ancestor of `a87672ab` (i.e. already present at reboot time)? |
|---|---|---|
| `current_task`/`discrete_task` stack overflow fix | `3afc5ea6` | **yes** — already fixed |
| `thermo_task` stack bump | `5b8fc53d` | **yes** — already fixed |
| watchdog reset-loop fix (impossible all-in-one-window check-in gate) | `25b6ef0b` | **yes** — already fixed |
| KilnFW/SaftyFW crash-logs/RTC-watchdog groundwork | `25be00f8` | **yes** — already fixed |
| **`link_task`/`update_task` stack raise** (`LINK_TASK_STACK_WORDS *6→*10`, `UPDATE_TASK_STACK_WORDS *3→*6`, heap `40K→56K`) | **`c27484a2`** | **NO — missing at reboot time**, committed later the same day (2026-09-10 13:13:50 -0700) |
| fatal-fault diagnostic self-erasure fix | `1496e288` | **NO — missing at reboot time** |

This is the single most load-bearing fact this investigation adds beyond the
two prior audits: **at the moment of the two reboots, `link_task` (which
owns the isolated UART channel carrying every config/command frame between
ESP and Pico, including whatever traffic a `profiles_start` heat-start
generates) was still running on the *pre-fix* `configMINIMAL_STACK_SIZE * 6`
(6144 B) stack**, which `link_task.c`'s own comment records as having
measured only 4736 B of margin against a check requiring ≥9472 B — i.e.
already known, one day later, to be inadequate by roughly 2x. `update_task`
was similarly under-provisioned (`*3`, 3072 B, needing ≥5072 B).

`link_task.c`'s own header comment independently documents that this exact
failure shape has happened before on this task: 2026-08-23, `*3` (a lower
multiplier than even the `a87672ab` build's `*6`) overflowed on hardware,
parking core 0 in `vApplicationStackOverflowHook()` with interrupts disabled,
freezing the tick count, and causing the **unfed hardware watchdog to reboot
the board repeatedly** — described in that comment as producing exactly this
class of symptom ("the board is in a reset loop"). That is not this
incident's own evidence, but it establishes that `link_task` stack
exhaustion causing a watchdog-attributed reboot is a **previously confirmed,
reproduced mechanism on this exact codebase**, not a hypothetical.

## 3. Candidate ranking against the evidence

**1. `link_task`/`update_task` stack overflow at heat start — best-supported, not proven.**
Evidence for: exact timing correlation (task was measurably under-provisioned
by ~2x on the failing build, fixed the next day, two independent clean runs
followed with no reboot); a previously-confirmed identical mechanism on the
same task (2026-08-23 incident, same file); `boot reason: watchdog` is
consistent with the un-fed-watchdog-after-stack-corruption mechanism
documented for that prior incident. Evidence against / gap: no SWD/forensic
read from the actual failing build (`a87672ab`) was taken *before* the
fix was applied — the SWD read available was taken *after* the board had
already advanced to `boot_id=60`, on the same un-fixed build, and came back
empty for the documented, unrelated reason (the `1496e288` erasure bug). No
stack high-water-mark or crash-tag evidence directly implicates `link_task`
specifically over `update_task` or some other path; the case rests on
provisioning math and prior-incident pattern-matching, not a captured
overflow.

**2. Watchdog reset-loop defect (impossible all-in-one-window check-in gate, `25b6ef0b`) — ruled out.**
This fix was already an ancestor of the failing `a87672ab` build (confirmed
via `git merge-base --is-ancestor`). It cannot be the cause of a reboot on a
build that already contains its fix.

**3. Brownout at relay-coil inrush — cannot be ranked; the hardware/firmware cannot distinguish it from a hang-triggered watchdog reset.**
`firmware/SaftyFW/src/boot_reason.c` and `.h` show the RP2040 boot-reason
latch here has exactly two independent facts: `watchdog_caused_reboot()` and
`watchdog_enable_caused_reboot()` (both pico-sdk booleans reading the
watchdog's own `REASON` register), plus an optional software-latched trip
reason in `scratch[0]`/`scratch[1]` that only exists if a safety guard
tripped first (it did not, per `trip_mask=0x0000`). The RP2040 has **no
brownout-detection register** distinct from the watchdog-forced-reset bit —
a genuine power-supply sag at relay-coil inrush and a task-stack-corruption
hang that starves the watchdog **produce the identical `REASON=0x00000001`
watchdog-forced signature** observed here. Nothing in this firmware or in
the RP2040 silicon itself could have told the two apart on this boot, so
brownout is neither confirmed nor excluded by the available evidence — it is
simply undiscriminated. Weak circumstantial evidence against it: this
board's relay coils are 5V (per `hardware/mainBoard/Regulators.kicad_sch`'s
rail description in `CLAUDE.md`), the Pico's SWD/link/thermocouple stack
sits on a physically separate board (`SaftyThermocoupleBoard`) from the
switched contactor loads, and no other symptom consistent with a supply sag
(ESP brownout, SX1509 fault, LCD glitch) was reported in either audit at the
same moment — but this is not a discriminating test, just an absence of a
second symptom.

**4. UART/link-driven fault (malformed or oversized frame from the ESP at profile-start triggering a link_task code path) — folds into candidate 1, not separable from it.**
The mechanism that would make a `profiles_start` command specifically trigger
this (rather than any other command) is plausibly a heat-start-specific
frame shape (e.g. a config/commit or CT-cal frame with a larger-than-usual
payload) hitting `link_task_handle_raw_frame()`'s documented worst-case
nested-buffer chain (`unstuffed[~520]` + handler payload + `link_send_frame()`'s
`raw[~259]` + `stuffed[~520]`, per `link_task.c`'s own comment). This is the
same stack-overflow mechanism as candidate 1, with the profile-start command
as the specific trigger frame rather than a separate cause; there is no
evidence separating "some frame overflowed the stack" from "specifically the
profile-start-adjacent frame did" versus a coincidentally-timed but
unrelated frame.

**5. SX1509 I/O-expander re-init timing — no supporting evidence found, not investigated further by either prior audit.** Named as an unranked candidate in the aborted-run document and never followed up; nothing in this session's evidence bears on it either way.

## 4. Current live board state (read 2026-09-11, this session, no reflash performed)

```
safety_get_fw_version(): Pico build c27484a2, built 2026-09-10 20:14:28Z,
  boot_id=38, config_version=144, config_crc=0x3768 (commissioned),
  protocol v12
safety_get_diag(): boot reason: watchdog | state armed | trip_reason 0
  | trip_mask 0x0000 | uptime 57182025 ms (~15.9 h) | context frames
  ok 17354, bad 0 | tx frames dropped 0
safety_get_status(): link up; armed; not tripped; thermocouple valid
safety_get_link_stats(): crc/framing errors 5, timeouts 9 (out of
  ~107k deframed frames), routed nowhere 1, length mismatch 11,
  crc mismatch 0, resync 0 — healthy for a link that has run 15.9 h
get_heap_status() (ESP): reset_reason='software (esp_restart)',
  uptime_s=35756, no unacknowledged crash banner
```

Interpretation: the Pico is currently running `c27484a2` — **exactly the
commit that raised `link_task`/`update_task`'s stacks** (candidate 1's fix)
— and has been up for ~15.9 hours with zero bad context frames and no
reboot. `1496e288` (the fatal-fault-diagnostic self-erasure fix) is
confirmed an ancestor of `c27484a2` (checked via `git merge-base
--is-ancestor`), so a *future* fatal fault on this running image would leave
a readable `scratch[5]` tag rather than being silently overwritten — closing
the forensic gap that prevented candidate 1 from being confirmed
retroactively. SaftyFW's source tree is only 4 commits ahead of `c27484a2`
as of this session, none of which touch `firmware/SaftyFW/src`. This is
consistent with, but does not prove, candidate 1: a board that would have
rebooted under the old provisioning has now run stably for far longer than
either of the two original reboot windows (30 s and ~2 min into a heat
start) without incident, across at least two heat-start events on record
(the two cplval45 runs) plus 15.9 h of otherwise-unlogged uptime.

## 5. Honest verdict on discriminating power

**The evidence cannot fully discriminate the candidates**, and this
investigation is not overriding that conclusion from the two prior audits.
What it adds:

- It **rules out** the previously-fixed watchdog reset-loop defect
  (`25b6ef0b`) as the cause, since that fix predates the failing build.
- It identifies, by direct commit-ancestry comparison (not available to
  either prior same-day audit, which did not have `c27484a2` to compare
  against yet at the time the aborted-run document was written), that
  `link_task`/`update_task` were the **only** stack-related fix that
  post-dates the failing build and pre-dates the confirmed non-recurrence —
  narrowing "three tasks were on bare `configMINIMAL_STACK_SIZE`" (as loosely
  stated in the later `cplval45_scale_vs_offset_2026-09-10.md` summary,
  which conflates several fixes applied together) down to the one fix whose
  timing actually matches the failure window.
- It confirms structurally, from `boot_reason.c`, that brownout and a
  watchdog-timeout-from-hang are **indistinguishable on this hardware today**
  — this is a hardware/firmware limitation, not a gap in this
  investigation's effort, and it means candidate 3 can never be excluded by
  boot-reason evidence alone, only by independent instrumentation (below).
- Two clean, multi-minute-to-multi-hour runs following the fix is
  correlational, not causal proof; per this task's own brief and both prior
  audits, that standard is deliberately not being overstated here.

**Ranking, honestly stated as a ranking of fit-to-evidence, not certainty:**
1. `link_task`/`update_task` stack overflow at heat start (best timing match, known-mechanism precedent, non-recurrence since fix)
2. Undiscriminated brownout at relay-coil inrush (cannot be excluded; no positive evidence either)
3. UART/link frame oversize as the specific trigger of (1) — same mechanism, unproven specific frame
4. SX1509 re-init timing — no evidence gathered either way
5. A general watchdog-check-in defect — ruled out (fix predates the failing build)

## 6. Minimal instrumentation to identify the cause if it recurs

1. **Read `scratch[5]` via SWD (or, once available, `safety_get_fw_version`/`safety_get_diag`-exposed diagnostics) immediately after any future Pico reboot, before starting any new activity that could trigger `watchdog_task`'s overdue-checkin tag.** With `1496e288` now on the running image, a genuine fatal-fault hook (`0xB4`/`0xA5`/`0xE3`) will no longer be silently overwritten — this closes the exact gap that made the original two reboots unrecoverable.
2. **Add a brownout-distinguishing read.** The RP2040 itself has no brownout detector, but the board's own supply rails are already monitored elsewhere in this project (5V/3.3V rails, `hardware/mainBoard/Regulators.kicad_sch`) — if any ADC or comparator on the Pico's own supply rail is accessible (even a coarse one), latching its value into an unused watchdog scratch register (there is exactly one, `scratch[7]`, noted as "the one free register" in `main.c`) at the same fatal-fault-hook sites would let a future incident distinguish "voltage was low right before the reset" from "voltage was fine, this was a pure software hang." Absent new hardware, at minimum log the ESP's own `get_heap_status`/12V-rail-adjacent telemetry (if any exists) at the same timestamp as any observed Pico reboot, to catch a system-wide sag that a Pico-only read cannot see.
3. **Capture `link_task`'s and `update_task`'s stack high-water mark continuously, not just via the static budget check.** `check_saftyfw_task_stack_budgets.py` is a build-time/static check; a live `stack_margin_register()`-style read (as already exists for the ESP side per `docs/audits/2026-09-08-stack-margin-audit.md`) for these two tasks specifically, polled during and immediately after every heat-start command, would catch a near-miss before it becomes a full overflow, and would have caught the original defect in minutes rather than requiring a day's worth of forensic reconstruction.
4. **Log the exact frame type/size in flight on `link_task` at the moment of any future reboot.** If `link_task`'s stack margin is ever seen to dip sharply coincident with a specific `SAFETY_CMD_*` frame, that closes the "which frame" question left open in candidate 4 above. A lightweight ring buffer of the last N frame headers (type + length) processed by `link_task`, read back over SWD or exposed via the existing diagnostic frame path, would suffice — no new hardware required.
5. **Before the next heat-start attempt of any kind, confirm via `safety_get_fw_version` that the running Pico commit is at or ahead of `c27484a2`** (the stack-raise fix) **and `1496e288`** (the diagnostic self-erasure fix) — both are prerequisites for this specific investigation's evidence chain to mean anything on a future incident. As of this session (`safety_get_fw_version` → commit `c27484a2`, `boot_id=38`), both are present.

## Summary for the record

The two original reboots were `boot reason: watchdog` with no safety trip
latched. The forensic tag that would have named the exact hook was destroyed
by a bug (`1496e288`, not yet fixed at the time) before it could be read.
Commit-ancestry analysis (new in this session) shows the only stack-related
fix that post-dates the failing build and pre-dates confirmed non-recurrence
is the `link_task`/`update_task` stack raise (`c27484a2`) — a task with a
**previously confirmed identical overflow-causes-watchdog-reboot-loop
mechanism** on this exact file (2026-08-23 incident). This is currently the
best-supported candidate, but it is corroborated only by timing and
precedent, not by a captured overflow event on the actual failing build.
Brownout at relay-coil inrush cannot be excluded because the RP2040's
`boot_reason` mechanism (`firmware/SaftyFW/src/boot_reason.c`) cannot
distinguish a supply sag from a hang-triggered watchdog timeout — both
produce identical `REASON=0x00000001` readings. The live board today runs
`c27484a2` (the candidate fix) and `1496e288` (the forensic-recovery fix),
has logged 15.9 hours of stable uptime with zero bad link frames, and has
survived at least two full heat-start events since the fix without a
reboot — consistent with, but not proof of, the top-ranked candidate.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
