# Review: F5HOLDER LOW fixes (86cad3ca8, 2026-10-11)

Scope: commit `86cad3ca8` on origin/dev, which fixes `REVIEW_F5HOLDER_2026-10-10.md`
LOW-1..LOW-4. Read-only review in a fresh worktree at origin/dev `2a4aa2db3`. Host tests and
negtests were run; no code was changed.

## Verdict

No HIGH. One MED: the LOW-4 per-tick ON retry can override a later segment's OFF on the same
relay. LOW-2 and LOW-3 are correctly closed. The new check works but has a blind spot.

## Findings

### MED-1: stale `wants_on` from an earlier still-active segment overrides a later OFF on the same relay

`io_segs_tick()` retries every runtime with `active && is_relay && wants_on && !state_on`. The
retry is placed before the `!active || blocking` continue, so it covers non-blocking segments
that stay active after the program moves on, and every warm-start-replayed segment
(`profile_executor_run.c` replay loop sets `blocking=true` and they stay active for the run).
`io_seg_start_since()` only resets its own runtime; it never clears an earlier segment's
request on the same relay, and `validate_io_segment()` is per-segment, so nothing forbids two
segments targeting one relay.

Sequence (normal run): segment A (non-blocking) asks relay N ON, the authority gate is closed,
so A keeps `wants_on=1, state_on=0`. Segment B asks relay N OFF and writes OFF. The gate later
opens and A's retry drives relay N ON, against B, the newest instruction. The same happens
after warm-start replay, where the replayed segments' requests are all still live.

Variant: if another segment's `io_seg_finish()` has already released relay N from
`claimed_relay_mask` and its `relay_authority` claim, A's retry turns the relay ON while no one
owns it, so a later finish or force-off may not cover it the way an owned relay would.

Before 86cad3ca8 a dropped ON stayed dropped, so this is new behaviour. Suggested fix: when a
segment starts on relay N, clear `wants_on` on every other active runtime targeting N (newest
request wins), or retry only when no later active runtime targets the same bit.

### LOW-1: the retry gate ignores a fresh Pico TRIPPED report

The retry is gated only by `relay_authority_on_blocked()`, which is true for a nonzero ESP
fault-source mask or a missing link, not for a Pico TRIPPED state. With a healthy link and no ESP
source set, the retry can close a RELAY_IO relay up to `WATCHDOG_CHECK_PERIOD_MS` (2000 ms)
before the watchdog faults the run on the trip. The tick already computes
`pre_lock_pico_tripped`. `io_seg_start` and `aux_apply_relay` have the same gap (pre-existing),
but the retry is a new instance of closing a relay after a block was seen. Suggested fix: skip the
retry while the Pico reports TRIPPED.

### LOW-2: retry runs before guard evaluation and before the blocking-segment finish

`io_segs_tick()` runs at the top of the RUNNING tick. On a blocking segment's last tick the
retry can switch ON and the finish later in the same tick switches it OFF again. A guard trip
later in the same tick also turns a fresh retry ON into an immediate OFF. Both give a pulse of a
few ms (contact wear, possible load inrush). Suggested fix: run the retry after the guards and
skip a runtime whose segment ends this tick.

### LOW-3: `check_app_hold_registration.ps1` excludes all of `drivers/control/`

The check skips `drivers/control/` and `test/` wholesale, but `autotune_engine.c` and
`zones_current_sweep_engine.c` also live there and could assert APP without registering. Negtest
C3 (unregistered APP assert added to `drivers/control/autotune_engine.c`) was MISSED. The check
also inspects asserts only, never deasserts. Suggested fix: exclude only the `profile_executor*`
files, or allowlist the executor's own APP sites by name.

### INFO

- `profile_executor_relay_io.c` near line 1173 still says "Not retried: a dropped ON stays
  dropped for the run", which now contradicts the code.
- A gate-dropped ON is still not surfaced in any status field, only in a log line.
- A failed retry write is silent and is retried every tick. This is acceptable, but unlogged.
- `wants_on`/`state_on` are not cleared in `io_seg_finish()`. This is harmless because every
  reader checks `active`, and run start plus `io_seg_start_since()` memset the runtime.
  Reset-one-side check: passes.
- `pe_app_release_recheck()` can briefly release and re-assert APP on the link when the bit was
  held longer than `SAFETY_FAULT_MIN_HOLD_MS`. That is a release followed by a microsecond-scale
  re-assert. Harmless.
- The LOW-4 test pairs `io_blocking=1` with `leave_on_at_end=1`, which the save validator
  refuses. It is unrealistic, but it still exercises `io_seg_finish(.., true)` correctly.

## Checked and correct

- Retry after pause/stop/abort/fault/segment end: impossible. Every non-RUNNING tick calls
  `io_segs_force_all_off(false)`, which clears `active`, and the retry requires `active`.
- Retry while APP or any ESP latch is held: blocked by `relay_authority_on_blocked()`, and the
  write goes through `kiln_io_owner_command_set_relay_mask_authorized_since()` with the
  off-epoch taken before the gate decision.
- System-mode gate: it guards external writers. Mid-run zone/aux reconfiguration is refused by
  that gate, and zone-owned and aux-bound RELAY_IO targets are refused at run start.
- LOW-2 window: closed. Both `safety_link_set_fault_source()` calls take `safety_lock` (a mutex),
  which orders `note()`-then-assert on app_main against deassert-then-recheck on the watchdog.
  Either the recheck sees `pe_app_owner_foreign`, or the boot assert happens after the deassert
  and wins. `volatile` is not needed for correctness on the dual-core S3 (the mutex provides the
  barrier) but is harmless.
- LOW-3: APP cannot stay latched forever. `clear_this_runs_faults()` skips the APP release only
  while guard-9 bookkeeping is pending and still zeroes `global_fault_source`. Once the
  bookkeeping merges, `relay_unknown_release_locked()` drops APP when no run, guard-9 pending or
  foreign holder keeps it. The foreign latch is sticky for the boot by design.
- Check discovery: `tools/check_app_hold_registration.ps1` matches `run_all_checks`' `check_*.ps1`
  glob. Its `# checkcache: ok` marker follows a BOM; checkcache reads with `ReadAllText`, so it is
  detected. The floor of at least 2 sites prevents a vacuous pass. Run directly: PASS (2 sites).
- Tests drive real executor functions (`clear_this_runs_faults`, `relay_unknown_release_locked`,
  `io_seg_start_since`, `io_segs_tick`, `io_seg_finish`) with fake link/io layers. Both new
  tests are registered in the runner.

## Tests and negative tests

Host tests: `build_host_tests.ps1 -Only prestart`, unmutated baseline inside the negtest run:
PASS.

Check negtest (`tools/negtest.ps1 -Preset check -PresetArg tools/check_app_hold_registration.ps1`):

| Id | Mutation | Result |
|---|---|---|
| C1 | `main.c` registration removed | CAUGHT |
| C2 | `main_control_bringup.c` registration removed | CAUGHT |
| C3 | unregistered APP assert in `drivers/control/autotune_engine.c` | MISSED (LOW-3) |
| C4 | registration moved 9 lines above the assert | CAUGHT |

Host negtest (`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`):

| Id | Mutation | Result |
|---|---|---|
| N1 | retry removed | CAUGHT |
| N2 | retry ungated (`relay_authority_on_blocked()` ignored) | CAUGHT |
| N3 | `state_on = false` on gate drop removed | CAUGHT |
| N4 | `pe_app_release_recheck()` made a no-op | CAUGHT |
| N5 | recheck call removed from `clear_this_runs_faults()` | CAUGHT |
| N6 | recheck call removed from `relay_unknown_release_locked()` | CAUGHT |
| N7 | `pe_app_guard9_pending()` dropped from the halt exclusion | CAUGHT |
| N8 | retry ignores `state_on` (writes every tick) | CAUGHT |

The unmutated baseline passed and every copy was removed. negtest reported an overall ERROR only
because the real worktree changed during the run: the reviewer was writing these two audit docs
at the time. No source file was touched.

## Fix status

| Finding | Status | Fix |
|---|---|---|
| MED-1 | FIXED | `io_seg_start_since()` and `io_seg_finish()` clear `wants_on` on every other active runtime targeting the same relay (`io_segs_clear_wants_on_others()`); tests: two segments, warm-start replay shape, finish-release variant |
| LOW-1 | FIXED (retry only) | `io_segs_retry_on(pico_tripped)` refuses while a fresh Pico TRIPPED report is pending. `io_seg_start` and `aux_apply_relay` still gate on `relay_authority_on_blocked()` only: neither has the report at hand under the executor lock (it is read outside the lock by rule), so no shared helper exists; left as is |
| LOW-2 | FIXED | Retry moved out of `io_segs_tick()` to `io_segs_retry_on()`, called after the blocking-segment finish and segment stepping, only while still RUNNING (it did NOT run after the guards; REVIEW_F5HOLDERFX2 LOW-1 moved it to the end of the RUNNING body, fixed in F5HOLDERFX2) |
| LOW-3 | FIXED | Check now scans `drivers/control/` except `profile_executor*.c`; resolves a local-variable mask to its assignment (accepted only if all constants are non-APP). Deasserts are not checked |
| INFO stale comment | FIXED | "Not retried" comment replaced |
| INFO status field | NOT DONE | not cheap; log line only |
