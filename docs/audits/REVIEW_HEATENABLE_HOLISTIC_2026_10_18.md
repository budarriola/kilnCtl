# Review: heat_enable.c holistic (2026-10-18)

Scope: `firmware/KilnFW/App/drivers/control/heat_enable.c` on origin/dev as one unit, after the
PF, PR, PQ, PZ and AP fix rounds, with its callers in `profile_executor.c` (watchdog tick
2625-2662), `profile_executor_run.c` (start), `profile_executor_status.c` (pause/resume/status) and
`safety_link_frames.c` (boot id / reboot seq). Review only, nothing fixed, no builds run.

Result: HIGH 0, MED 1, LOW 2, INFO 4. No sequence grants heat before a verdict, and none
deadlocks heat for good. The last release always clears `reboot_hold`, and an acquire consumes a latch.
One sequence breaks the repeated-reboot rule (HH-01).

## State table

Watchdog order each tick: `note_boot_id_seen` -> `note_pico_boot` -> `note_pico_trip_info` ->
`note_pico_state` -> hold/unseen pause -> `reconcile`.

| Event | boot_id_unseen / first_sight_done | verdict_pending | classify_pending / since_ms | reboot_had_claim | latch (fatal_latched, lost_trip_only, expirable) | reboot_hold | benign window |
|---|---|---|---|---|---|---|---|
| init | 1 / 0 | 0 | 0 / 0 | 0 | 0,0,0 | 0 | last=0 |
| first boot id (PR-01/PQ-01) | 0 / 1 | 1 | 1 if held (PZ-02); since 1 if 0 | 1 if held | untouched | untouched | untouched |
| new reboot seq | - | 1 | 1 if held; since=now (0 -> 1) | OR held | untouched (never downgraded) | untouched | untouched |
| DIAG, no claim, fatal bits | - | 0 | 0 / kept | 0 | 1,0,0 | 0 | not updated |
| DIAG, no claim, lost trip | - | 0 | 0 / kept | 0 | 1,1,(!had_claim and bits) | 0 | not updated |
| DIAG, no claim, benign | - | 0 | 0 / kept | 0 | unchanged | 0 | not updated (HH-01) |
| DIAG, claim, fatal/lost/repeated | - | 0 | 0 / kept | 0 | untouched | 1, granted=0, release owed | updated if benign and since != 1 |
| DIAG, claim, benign | - | 0 | 0 / kept | 0 | untouched | 0 | last=since unless since == 1 (AP-01) |
| acquire (was_empty) | - | - | 1 if verdict_pending; since 0 -> 1 | 1 if verdict_pending | consumed -> hold | 1 if latched | - |
| last release | - | kept | 0 / kept | kept | kept | 0 | kept |
| TRIPPED then ARMED, same seq, no claim | - | - | - | - | expires if expirable and cleared bits cover lost bits | - | - |

Acquire withholds on unseen, classify_pending or hold, unless a grant already stands (HH-02).
Reconcile retries only when pending, held, not granted, and none of hold, classify_pending or unseen.

DIAG verdicts: fatal = WATCHDOG/BROWNOUT/STACK_OVERFLOW/MALLOC_FAILED/ASSERT_FAILED. Lost trip
means the reboot followed a TRIPPED DIAG and no fatal bit is set. Repeated means a benign reboot within 15 min of
`last_benign_reboot_ms`.

Sequences checked:
- Heat before a verdict: acquire and reconcile both gate on `boot_id_unseen`, `classify_pending` and `reboot_hold`.
  `send_enable` undoes a grant that lands under hold or classify_pending. The PZ-02 held-while-unseen claim is
  re-armed at first sight. No path sends enable before a DIAG of the current boot. The one gap is HH-02: an
  existing grant is kept, nothing new is sent.
- Permanent deadlock: none. `reboot_hold` clears on the last release, and the latch is consumed by the next
  acquire. An undecided verdict with no DIAG stays undecided, but stop and restart re-arm it and a DIAG
  resolves it (HH-05).
- Benign auto-resume: the claim branch with since != 1 grants through the F1 resend (grant kept) or through
  reconcile (pending). Tested by `test_ap01_first_sight_not_repeated_benign` and the PZ-02 test.
- Fatal holds: the claim branch holds. Without a claim, the reboot latches and the next acquire holds. Tested.

## Findings

### HH-01 MED: benign reboots classified without a claim never enter the SR-11 window
`heat_enable.c:833-867`. The claimless branch resolves a benign verdict without reading or writing
`last_benign_reboot_ms`. Only the claim branch (884-894) counts. Two sequences escape the
repeated-benign hold:
1. The Pico reboots (benign) while idle, a firing starts, and the Pico reboots (benign) again within 15 min. The
   second reboot is the first counted, so heat auto-resumes.
2. The Pico reboots during a firing, and the claim is released (pause or stop) before the new boot's DIAG
   arrives. `he_release_common` clears `classify_pending`, so the DIAG resolves in the claimless branch and the
   reboot is not counted. A resume plus one more benign reboot inside 15 min then auto-resumes.

This breaks the owner rule that a repeated reboot holds heat. SR-11's fix text speaks of counting "per heat claim".
The brownout argument, though, does not depend on a claim being held. Fix: in the claimless benign branch, apply
the same window update (skip since == 1). Optionally latch fatal when repeated. Test gap: no test covers either
sequence.

### HH-02 LOW: an existing grant survives the undecided window
When a reboot is noted with a claim held and `granted == true`, `note_pico_boot` sets classify_pending
but leaves `granted` set. Until the DIAG arrives, `heat_enable_is_granted()` reports true, and an acquire by
a second claimant takes the `already_granted` branch (`heat_enable.c:534-561`) and returns true. Nothing is sent,
and the new Pico boot keeps K4 open, so no heat flows. Status and callers do see "granted" before any verdict.
A fatal verdict then clears it. Untested.

### HH-03 LOW: autotune is not aborted on boot_id_unseen
`profile_executor.c:2655-2657` pauses the firing with `pico_boot_id_unseen` but does not call
`autotune_engine_abort_bounded`, unlike the hold and unconfirmed branches. An autotune claim taken before the
first boot id stays withheld (safe) and runs cold without a reason shown. PZ-02 then classifies it correctly
at first sight. A fail-closed UX gap.

### HH-04 INFO: since_ms / last_benign sentinels share the tick domain
`reboot_classify_since_ms` uses 0 = none and 1 = first-sight/re-arm (AP-01). `last_benign_reboot_ms` uses 0 = none.
A real reboot noted at tick 0 or 1 ms, or after a 49.7-day wrap, collides with these values. At worst the reboot is
not counted, or a wrap-straddling pair is counted. Negligible.

### HH-05 INFO: an undecided verdict never times out (by design)
With a claim held and no DIAG from the new boot, heat stays withheld with no pause, only the `reboot_undecided`
status (`profile_executor_status.c:634`). This is fail-safe and not a deadlock, because stop and restart re-arm
the verdict.

### HH-06 INFO: since_ms is never cleared after a verdict
The first-sight and acquire re-arm write `since_ms = 1` only when it is 0. This is correct today only because
`note_pico_boot` returns early while unseen, so no real reboot time can come before first sight, and a re-arm always
follows a reboot that stamped a fresh time. A later change that clears `boot_id_unseen` elsewhere would silently
feed a stale time into the window.

### HH-07 INFO: test coverage per edge
Covered: first sight with and without a claim (PQ-01, PZ-02), the AP-01 sentinel, SR-11 inside and outside the
window, T3 lost trip, PR-02 latch expiry and non-expiry, MED-5 re-arm after release, late fatal DIAG, and send
under hold. Not covered: HH-01 (both sequences), HH-02 (second-claimant acquire while undecided), and a fatal latch
consumed by an acquire at the same time as a pending re-arm.
