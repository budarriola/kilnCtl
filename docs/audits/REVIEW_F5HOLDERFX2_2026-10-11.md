# Review: F5HOLDERFX fixes (8c833beb4, 2026-10-11)

Scope: commit `8c833beb4` on origin/dev, which fixes `REVIEW_F5HOLDERFX_2026-10-11.md`
MED-1, LOW-1..3 and the stale-comment INFO. Read-only review in a fresh worktree at origin/dev
`bdceee5a8`. No source was changed. Host tests and the check were negative-tested.

## Verdict

No HIGH and no MED. MED-1 (stale `wants_on` overriding a later OFF) is fixed and tested. LOW-1
is fixed for the retry. LOW-2 is only half fixed: the retry now runs after the blocking finish and
segment stepping, but still runs before the thermal guards, contrary to its comment and the fix
table. The new check variable resolution can be fooled in three simple ways. No path was found
that leaves a relay ON when the run wants it OFF for longer than one tick.

## Findings

### LOW-1: the retry still runs before the thermal guards (LOW-2 of the prior review only half fixed)

`io_segs_retry_on()` is called at `profile_executor.c:1248`, after segment stepping. The
per-zone `thermal_guard_tick()` / `escalate_guard_trip()` run later in the same tick, at about
line 1792, and `exec_mode_state_check()` runs later still. A global guard trip in that tick calls
`io_segs_force_all_off(false)`, so a relay the retry just closed opens again a few ms later: the
same ON/OFF pulse the prior LOW-2 described for guards. The ordering against the blocking-segment
finish is fixed, because `io_seg_finish()` at line 1077 runs first. The new code comment ("after
guards and the blocking finish") and the prior review's fix table ("called after the guards") are
both wrong. This is not a regression, because the retry ran even earlier before. Fix: move the
call to the end of the RUNNING body, after the zone loop and the aux tick, just before the lock is
released, still gated on `state == RUNNING`. Then fix the comment.

### LOW-2: `check_app_hold_registration.ps1` variable resolution is easy to fool

The new rule takes the first `\bNAME\s*=\s*...;` match in the whole file. It accepts the variable
if that one assignment names a non-APP `SAFETY_FAULT_SRC_*` constant. Negtest against
`drivers/control/autotune_engine_guard.c` (`source`):

| Id | Mutation | Result |
|---|---|---|
| C1 | `source \|= SAFETY_FAULT_SRC_APP;` added before the assert | MISSED (compound assignment not seen) |
| C2 | later `source = SAFETY_FAULT_SRC_APP;` reassignment | MISSED (only the first assignment is read) |
| C3 | APP placed in the declaring ternary | CAUGHT |
| C4 | declaration becomes `(SAFETY_FAULT_SRC_THERMAL_SANITY \| at_extra_src())` | MISSED (non-constant term accepted) |

The match also ignores function scope, so a same-named variable in another function can satisfy
it. A `NAME == X` comparison can match too, because `\s*=\s*([^;]+)` absorbs the second `=`.
None of the current sites is wrong. The only resolved site today
(`autotune_engine_guard.c:188`) is a THERMO/THERMAL_SANITY ternary. Fix: accept a variable only
if every assignment to it in the file, `=` and `|=`, consists solely of non-APP
`SAFETY_FAULT_SRC_*` constants, `?`, `:`, `|`, parentheses and identifiers used as conditions.
Otherwise fail closed, or allowlist the site by file and line.

### LOW-3: the declared "not done" Pico-TRIPPED gap is cheaper to close than stated

The fix table says `io_seg_start` and `aux_apply_relay` cannot see the fresh Pico TRIPPED report
because it is read outside the executor lock. That is true only for the warm-replay start in
`profile_executor_run.c`, which has just passed `relay_authority_start_blocked()`. Both the normal
`io_seg_start()` (`profile_executor.c:1049`) and `profile_executor_aux_tick()` (line 1992) run
inside the same RUNNING tick that already holds the `pre_lock_pico_tripped` local. The real risk:
while the Pico reports TRIPPED but no ESP fault source is set yet, a RELAY_IO segment start or an
aux rule can close a spare relay. That relay may not sit behind K4 (the aux code itself says "never
assume K4 covers them"), and it stays closed until the watchdog faults the run, at most
`WATCHDOG_CHECK_PERIOD_MS` (2 s) later. The risk is bounded but real. Fix: pass the local down and
treat it like `relay_authority_on_blocked()` at both sites.

### LOW-4: test coverage gaps found by negtest

| Id | Mutation | Result |
|---|---|---|
| H1 | clear removed from `io_seg_start_since()` | CAUGHT |
| H2 | clear removed from `io_seg_finish()` | CAUGHT |
| H3 | `pico_tripped` ignored in `io_segs_retry_on()` | CAUGHT |
| H4 | retry ungated (authority check removed) | CAUGHT |
| H5 | `io_segs_retry_on()` call removed from the executor loop | MISSED |
| H6 | `io_segs_clear_wants_on_others()` drops the `target` match (clears every relay) | MISSED |

H6 matters for the question of whether the fix can wrongly drop a legitimate ON. Code reading
shows the target match is correct today, but no test pins it: a pending ON on relay 2 must survive
a segment start or finish on relay 3. H5 is the usual limit of tests that do not drive the tick
loop. The placement (LOW-1 above) is unverified by any test, and so is the `state == RUNNING` gate.

### INFO

- Newest-wins can drop a later segment's pending ON when an earlier segment on the same relay
  finishes. Example: A is ON (applied) and non-blocking, and B is ON (gate-dropped) and started
  later. When A's hold expires, A forces the relay OFF and clears B's `wants_on`. This is
  conservative (OFF), and it matches the existing behaviour, where any finish on a relay forces it
  OFF even while another segment's applied ON is still active. It is not unsafe, but it is a
  semantic the plan does not state.
- A warm-replayed segment stays active (`blocking = true`) for the whole run. A gate-dropped
  replayed ON is therefore retried at any point in the run until a later segment on that relay
  clears it. This matches the replay rule ("stays as commanded until the run ends").
- `pre_lock_pico_tripped` is sampled before `xSemaphoreTake(s_exec.lock, portMAX_DELAY)`, so it
  can be stale by the lock wait. Every other reader of it has the same window, and the
  off-epoch-guarded write still refuses a write after any force-off.
- Deasserts are still not checked. The only non-executor deassert
  (`autotune_engine.c:1387`, `s_at.global_fault_source`) is written only from the THERMO /
  THERMAL_SANITY ternary, so it cannot clear the APP boot latch today.

## Checked and correct

- MED-1: `io_seg_start_since()` clears every other active relay runtime on the same target before
  its own memset-and-start. `io_seg_finish()` does the same after it releases the bit. Replay
  starts segments in profile order, so the latest replayed command wins, and the first
  normal-path start on that relay supersedes all of them.
- Pause, abort, halt, fault, DONE: every non-RUNNING tick and every terminal path calls
  `io_segs_force_all_off()`, which clears `active`. The retry needs `active`, is called only while
  `state == RUNNING` under `s_exec.lock`, and samples the off-epoch before its gate decision, so a
  concurrent force-off makes `..._authorized_since()` refuse.
- A blocking segment that ends this tick is finished (line 1077) before the retry, and a
  non-blocking one whose hold expired is finished by `io_segs_tick()` at the top of the tick.
- `io_segs_tick()` no longer writes anything. The LOW-2 test pins this.
- Check scope: `drivers/control/` is scanned except `profile_executor*.c`. Prior negtest C3 (an
  unregistered APP assert in `autotune_engine.c`) would now be scanned, and C3 above was CAUGHT.

## Tests run

- Host: `build_host_tests.ps1 -Only prestart` unmutated baseline (inside negtest): PASS.
- Host negtest (`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`): H1-H4
  CAUGHT, H5-H6 MISSED (table above).
- Check negtest (`-Preset check -PresetArg tools/check_app_hold_registration.ps1`): baseline PASS,
  C3 CAUGHT, C1/C2/C4 MISSED.
- Every negtest copy was removed and the real tree was unchanged.
