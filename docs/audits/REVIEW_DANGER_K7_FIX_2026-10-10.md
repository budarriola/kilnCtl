# Review: danger-mode interlock and K7 relay IO round 2, 2026-10-10

Reviewer: Opus, review only (no fixes in this commit).
Reviewed at dev tip `1d3f7dac8`.

Range:

- A. Danger-mode interlock: `6d4cc7ab6` (code), `2ae084360` (docs). Fixes LCD
  review R1/R2/R3/R9 and host-test campaign F5-1/F6-1: claim-first-then-recheck
  on both sides (run start and danger-mode entry), `danger_mode_blocks_start()`
  failing closed.
- B. K7 relay IO round 2: `bcf448e71` (code), `fbf6e1070` (stack re-pin),
  `45e13408e` (docs), follow-up `1453133ce` (relay-authority allowlist entry for
  `relay_unknown_prelock_check`, result capture of the F6-1 re-entry release).
  Fixes K7 review HIGH-1, MED-1/2/3, LOW-1/4/5, NIT.

Files: `firmware/KilnFW/App/drivers/safety/danger_mode.c`,
`control/profile_executor.c`, `control/profile_executor_run.c`,
`control/autotune_engine.c`, `owners/kiln_io.c`, `owners/kiln_io_owner.c`,
`firmware/KilnFW/App/test/test_kiln_io_sx_fake.c`,
`test_profile_executor_prestart.c`, the danger-mode tests.

Looked for: races, deadlock between the kiln_io lock, the owner task and the
LVGL D/C line, a relay claimed ON against undriven pins, a relay left energised
after a fault, fail-closed defaults.

## Verdict

Both batches do what they claim on the paths the tests cover. The two-sided
Dekker recheck is correct: the run side publishes its heat claim under the
relay_authority spinlock and then reads `danger_mode_blocks_start()`; the danger
side publishes `window_open` under its mutex and then reads
`relay_authority_heat_run_active()`, so at least one side always refuses.
`danger_mode_blocks_start()` reads "blocked" on a lock timeout. The system-mode
gate refusal is scoped to profile and autotune start only, which is right. No
lock cycle was found between `io_lock`, the SX1509 driver lock, `s_exec.lock`
and the danger-mode mutex; LVGL's D/C writes wait a bounded 6500 ms per toggle
and fail rather than deadlock.

No HIGH or MED findings. Five LOW, one NIT, two advisories. The one worth fixing
first is F1: a failed 50 ms re-take on the danger-mode refusal path leaves the
window open while a firing holds heat, and the 5-minute expiry then cuts that
firing's relays.

Separately, the fresh target build fails the httpd stack-budget check
(`profile_exec_start_post_handler` 4512 B against a 4448 B ceiling). The growth
is not in these batches (see "Stack").

## Findings

### F1 (LOW): failed re-take leaves the danger window open under a running firing

`danger_mode.c:101-116`, `danger_mode_request_start()`. The window is published
(lines 84-93), then the heat claim is read. If a run holds it, the rollback
re-takes `s_dm.lock` with a 50 ms timeout (line 103). If that take fails, the
function logs the refusal and returns false with nothing restored:
`window_open` stays true, and in the already-open case the deadline extension
also stays.

Consequences while the firing runs:

- `danger_mode_active()` reads true, so the diagnostics danger relay route
  bypasses its gates and the uart_bridge link-loss drop is suppressed.
- At expiry (`DANGER_MODE_WINDOW_MS`, 5 min), `danger_mode_task` clears the
  window and calls `kiln_io_owner_command_all_relays_off()` plus
  `safety_link_request_enable(false)` (around lines 368-385). That cuts the
  firing's heat. The run sees it as relay feedback, not as a refusal it caused.

Reachability: the only other holders of `s_dm.lock` are short (status reads,
the expiry task), so a 50 ms miss needs a priority inversion or a stalled
holder. Rare, but the result is a silent interference with a firing.

Fix direction: on a claimed heat run, retry the take until it succeeds (block,
or bounded loop with a log), or keep the window publish and the rollback inside
one critical section that cannot fail. Restore `deadline_ms` in the
already-open case too.

### F2 (NIT): microsecond window where danger mode reads active during a start

Between the publish (line 90) and the rollback (line 110), any reader of
`danger_mode_active()` sees an open window while a run holds heat. Inherent to
publish-then-recheck and harmless for the start race itself; noted only because
`danger_mode_active()` gates the diagnostics relay route and the link-loss
drop, not just starts. No action needed unless F1's fix changes the shape.

### F3 (LOW): unlocked fail-safe all-off can report OK over an in-flight ON

`kiln_io.c:565-597`, `kiln_io_all_relays_off()`. After the 2000 ms
`KILN_IO_FAILSAFE_LOCK_WAIT_MS` wait, it does one unlocked
`SX1509_write_masked(..., 0)`, sets `relay_state_unknown = true`, and on a
successful write sets `relay_shadow = 0` and returns `ESP_OK`. The lock holder
may be mid-way through an ON write that the SX1509 driver lock serialises AFTER
the fallback's OFF. The coil can then be ON while the caller got `ESP_OK`
(`kiln_io_owner.c` notes the off-tracker on that OK) and the shadow says 0. The
writes to `relay_shadow` and `relay_state_unknown` also race the holder.

Mitigated: `relay_state_unknown` stays raised, ON is refused while it is set,
and the watchdog retries a locked, verified all-off every period (about 1 s).
So the window is bounded by the holder's own write and the next watchdog pass.

Fix direction: return a distinct error (not `ESP_OK`) from the unlocked path so
callers do not record a verified OFF; leave the shadow alone there.

### F4 (LOW): relay-unknown release runs before the guard-9 assert in the same pass

`profile_executor.c:2314` vs `2315-2329`. `watchdog_task_entry()` calls
`relay_unknown_release_locked()` before the guard-9 bookkeeping block.
`guard9_prelock_check()` asserts APP on the link but `global_fault_source` only
gains APP later, in `guard9_assert_stale_tick_fault()`. If guard 9 has just
fired and the relay-unknown hold releases in the same pass, the release sees no
APP in `global_fault_source` (line 2183) and deasserts APP on the link; the
guard-9 block then reasserts it a few statements later. The Pico can see a
transient APP drop during a guard-9 trip.

Fix direction: move `relay_unknown_release_locked()` after the guard-9 block, or
have the release also check `tick_stale || s_guard9_bookkeeping_pending`.

### F5 (LOW): SAFETY_FAULT_SRC_APP is shared with no ownership count

APP is asserted by guard 9, by the relay-unknown hold and by the boot safe-state
(`main_kiln_enter_safe_state()` in `main_bridges_bringup.c`, and two sites in
`main_network_http.c`). Each releases it on its own condition:

- `clear_this_runs_faults()` (`profile_executor_status.c`, via halt) deasserts
  APP while the relay-unknown hold is active. `s_relay_unknown_fault_asserted`
  stays true, so the hold never reasserts it.
- `relay_unknown_release_locked()` can clear the boot-time safe-state APP latch
  (it only checks `global_fault_source`).

The kiln_io-level ON refusal while `relay_state_unknown` is set still holds, so
no relay is energised on uncertainty; what is lost is the Pico-side fault. This
is the "reset one side of a pair" class (CLAUDE.md).

Fix direction: a per-owner bitmask for APP holders, with the link bit derived
as "any owner holds it".

### F6 (advisory): relay_state_unknown does not fault the run

A raised `relay_state_unknown` asserts APP and refuses ON, but the run stays
RUNNING with no heat until the chip answers. That is a deliberate choice in
`bcf448e71`; flagging it so an owner decision exists. Also, the CT-sweep relay
claim is not part of the danger-mode heat exclusion; it is not a firing, but
nothing stops a danger window opening over a sweep.

### MED-3 bound (advisory): the dead-bus path is still long

With `SX1509_TIMEOUT_MS` 1000 and `I2C_WRITE_RETRY_ATTEMPTS` 5,
`write16_verified` is 5 outer by 5 inner attempts. On a dead bus,
`kiln_io_all_relays_off_locked()` makes about 26 transfers (25 for the write,
one read for the resync) before returning: about 26 s worst case with 1 s
stuck-bus timeouts. The MED-3 skip removes the reset/re-init cycle, as claimed.
During that time the owner holds `io_lock`. LVGL D/C toggles time out at 6500
ms each (no deadlock, just failed draws). The watchdog can hold `s_exec.lock`
for the 2 s fail-safe wait, then up to 6 s on the SX1509 driver lock, then the
unlocked write's own retries (pre-existing).

### Test gaps

- The executor relay-unknown test (`test_relay_state_unknown_is_a_fault` in
  `test_profile_executor_prestart.c`) calls the helpers directly. Nothing covers
  the `watchdog_task_entry()` wiring or its ordering (F4). See N1.
- MED-3's bound is pinned (`dead_attempts <= 30`, no reset attempts) and LOW-4
  is covered inside `test_por_then_off_commands_are_honest`; see the table for
  what the mutations show.

## Stack

Fresh target build ELF on dev tip `1d3f7dac8`, measured with the standing
stack-budget checks (static call-graph worst case).

| Task | Worst-case path | Ceiling | Stack | Free (honest) | Result |
|------|-----------------|---------|-------|---------------|--------|
| httpd | 4512 B (`profile_exec_start_post_handler`) | 4448 | | | FAIL |
| executor | 3408 B | 3408 | 6144 | 1516 B (24.7%) | OK, LOW margin |
| kiln_io_owner | 2160 B | 2160 | | 1636 B (39.9%) | OK (indeterminate lower bound) |
| lvgl | 7520 B | 7520 | | 2420 B (23.6%) | OK |
| profile_exec_wdt | 2736 B | 2736 | | 3108 B (50.6%) | OK (indeterminate) |
| danger_mode | 2256 B lower bound | | 3072 | 16.8% | OK, LOW margin |
| http_async_job | 7712 B | 7712 | | | OK |

httpd FAIL: the deep path is `profile_exec_start_post_handler`, then
`readiness_gate_collect`, then the CT verify, then the zones cfg_fs raw load,
then the rejected-file preserve, then `cfg_fs_read`. Not attributable to these
batches: the `profile_executor_run` frame is unchanged at 896 B against an
archived ELF built before them, and `readiness_gate_collect` grew from 256 B to
272 B. The whole-task check reports all 33 tasks INDETERMINATE (OK).

## Negative tests

`tools/negtest.ps1`, host tests `-Only
'kiln_io_sx_fake|profile_executor_prestart|danger_mode|autotune_engine_prestart|kiln_io_owner'`,
base `1d3f7dac8`. CAUGHT means a test failed with the mutation applied.

Baseline passed (44.9 s). 9 of 12 mutations CAUGHT, 3 MISSED.

| Id | Mutation | Covers | Result |
|----|----------|--------|--------|
| N1 | watchdog loop no longer calls `relay_unknown_prelock_check()` | MED-1 wiring | MISSED |
| N2 | relay-unknown release ignores the guard-9 APP bit | MED-1 release | CAUGHT |
| N3 | MED-3 skip removed (always reset/re-init) | MED-3 bound | CAUGHT |
| N4 | MED-3 skip widened to any unreadable chip (drops the `ESP_ERR_INVALID_RESPONSE` exception) | MED-3 | MISSED |
| N5 | no re-init after a failed write finds the relay pins as inputs | LOW-4 | CAUGHT |
| N6 | no resync after a failed expander reset | LOW-5 | CAUGHT |
| N8 | owner skips the off-tracker note after a successful SX_RESET | owner, LOW-5 | MISSED |
| N9 | unlocked fail-safe fallback clears `relay_state_unknown` on a good write | MED-2 | CAUGHT |
| N10 | `danger_mode_blocks_start()` fails open on lock timeout | R3 | CAUGHT |
| N11 | danger-mode entry drops its heat-claim recheck | R1/R2 | CAUGHT |
| N12 | profile start drops its late danger recheck | R1/R2 | CAUGHT |
| N13 | autotune start drops its late danger recheck | R1/R2 | CAUGHT |

MISSED, in order of value:

- N1: the executor test calls the relay-unknown helpers directly, so deleting
  the call from `watchdog_task_entry()` passes. A test that runs one watchdog
  pass with `relay_state_unknown` raised and checks APP is asserted would catch
  N1 and F4.
- N8: no owner-level test checks that a successful SX_RESET records all relays
  off in the off-tracker.
- N4: no test drives a verify mismatch (`ESP_ERR_INVALID_RESPONSE`) followed by
  an unreadable chip, the one case where the exception in the MED-3 condition
  changes behaviour.

The negtest JSON reported `real_tree_unchanged: false`. The only change in the
worktree during the run was this review document being written; the shared
main tree was unchanged (checked with `git status --porcelain` before and
after).
