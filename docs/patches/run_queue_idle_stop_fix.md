# run_queue.py: raise immediately on a mid-run "idle" sample

**Status:** NOT applied. `tools/PcTools/src/kilnctrl/run_queue.py` is
currently driving six unattended firings on real hardware
(2026-08-31/09-01 campaign) and must not be edited until that finishes.
Apply `run_queue_idle_stop_fix.patch` (same directory) the moment it does,
then run:

```
python -m pytest tools/PcTools/tests -q
RUN_IDLE_STOP_FIX_TESTS=1 python -m pytest tools/PcTools/tests/test_run_queue_idle_stop_fix.py -q -v
```

Both should be green -- the first because the patch changes nothing about
existing behavior (`test_run_queue.py`'s 35 cases all still pass, verified
against a patched scratch copy), the second because it flips the two new
tests in `test_run_queue_idle_stop_fix.py` from red to green. Then delete
the `@unittest.skipUnless(_RUN_FIX_TESTS, ...)` decorators (or the whole
`_RUN_FIX_TESTS`/`_SKIP_REASON` gate) from that test file -- it no longer
needs to hide from `run_all_checks.ps1`.

## The bug

`run_queue.py`'s docstring, at (current, unpatched) lines 104-106 justifies
excluding `"idle"` from `_TERMINAL_STATES` by claiming:

> profile_executor_status.c never transitions RUNNING/PAUSED back to IDLE,
> only to DONE or FAULTED.

That is false. `firmware/KilnFW/App/drivers/profile_executor_status.c:77`
sets `s_exec.state = PROFILE_EXEC_IDLE;` directly out of RUNNING/PAUSED on
an operator Stop, and that file's own comment at lines 65-67 names this
exact case:

> PID_EXPANSION_PLAN.md Phase 7a: the OTHER ending the tick loop's DONE/
> FAULTED branch doesn't see -- an operator Stop straight out of RUNNING or
> PAUSED.

`"idle"` correctly stays out of `_ACTIVE_STATES` (state 0 is genuinely
ambiguous pre-start) and it should also correctly stay out of
`_TERMINAL_STATES` (an idle sample alone still isn't evidence the run
reached DONE/FAULTED). The bug is not the exclusion -- it's that nothing
else in the module recognizes a **post-start** idle sample as its own,
distinct, actionable condition. `_run_is_terminal` returns `False` for it,
so `_poll_capture_until` just keeps polling until `run_timeout_s`
(`total_planned_s * 1.25 + 600s` -- ~72 minutes for profile 7) expires,
then raises a generic "still not terminal after Ns" `RunQueueError`. Safe,
loud, just needlessly slow.

## The fix

Three changes to `tools/PcTools/src/kilnctrl/run_queue.py`, all in
`run_queue_idle_stop_fix.patch`:

1. **Correct the docstring** at `_TERMINAL_STATES` (current lines 97-107):
   replace the false "never transitions... back to IDLE" claim with an
   accurate description citing `profile_executor_status.c:65-77`, and
   explain that the real fix is a dedicated post-start idle check, not
   folding `"idle"` into `_TERMINAL_STATES`.

2. **Add `check_not_idle_after_start(exec_body)`** (new pure function,
   next to `check_no_fault`/`check_not_faulted`): raises `RunQueueError`
   with a clear "stopped externally" message if `exec_body["state"]` is
   `"idle"`. Pure, fixture-testable, no HTTP -- same shape as its two
   neighbors.

3. **Wire it into the main run-capture poll only.** `_poll_capture_until`
   gains a `raise_on_idle: bool = False` parameter; when `True`, it calls
   `check_not_idle_after_start(exec_body)` on every sample, right after
   `check_no_fault`/`check_not_faulted`. `run_entry`'s main run-capture
   call (`_poll_capture_until(cfg, fh, _run_is_terminal,
   deadline_s=run_timeout_s, ...)`) passes `raise_on_idle=True`; the
   cooldown capture call is left untouched (`raise_on_idle` defaults
   `False`), because during cooldown the board settling to
   idle/rested is the expected, desired end state, not an error.

   The pre-start window is untouched: `_wait_until_run_active_or_terminal`
   already owns "idle before the run is confirmed to have started" and is
   not changed at all -- it still polls until the state leaves idle
   (RUNNING/PAUSED) or reaches a terminal state, within
   `start_confirm_timeout_s`. `raise_on_idle=True` only ever applies to
   samples taken *after* that function has already returned successfully,
   so a run that starts, is confirmed running, and is stopped is
   distinguished cleanly from a run that simply hasn't started yet.

## Tests

New file: `tools/PcTools/tests/test_run_queue_idle_stop_fix.py`. Does not
modify `test_run_queue.py`. Drives the real `run_entry()` entry point (not
a private helper) through `_ScriptedTransport` with the exec-state
sequence `running -> idle` (first `GET /api/profile_exec` during
start-confirm reports `running`; every one after, during the main
run-capture loop, reports `idle` -- exactly the operator-Stop shape from
`profile_executor_status.c:65-77`).

Two assertions, from two angles:

- `test_idle_after_confirmed_running_raises_immediately_with_a_clear_message`
  -- the raised message mentions `"idle"` and `"stopped externally"`, and
  explicitly does **not** contain the old timeout-shaped text
  `"still not terminal after"`.
- `test_idle_after_confirmed_running_raises_before_any_sleep` -- behavioral,
  not string-dependent: `cfg.sleep()` is the last thing each
  `_poll_capture_until` iteration does, so a raise on the very first
  main-loop sample means `transport.sleeps == []`.

**Proof of red**, run just now against the unpatched module
(`RUN_IDLE_STOP_FIX_TESTS=1 python -m pytest
tools/PcTools/tests/test_run_queue_idle_stop_fix.py -q -v`):

```
FAILED ...test_idle_after_confirmed_running_raises_before_any_sleep
  AssertionError: Lists differ: [1.0, 1.0, 1.0, 1.0, 1.0, 1.0] != []
FAILED ...test_idle_after_confirmed_running_raises_immediately_with_a_clear_message
  AssertionError: 'stopped externally' not found in "profile_exec
  state='idle' is still not terminal after 6s -- treating a run that
  outlives its own timeout as an error, not a completed run"
2 failed in 0.09s
```

**Proof of green**, run against an isolated scratch copy of the source
tree with the patch applied: both tests pass, and all 35 pre-existing
`test_run_queue.py` cases still pass unmodified against the same patched
copy. The full `tools/PcTools/tests` suite (1296 tests) is green today,
unpatched, with the two new tests skipped by default (`_RUN_FIX_TESTS`
gate) -- see the skip reason string in the test file for how to un-skip.

## Other places checked for the same false assumption

Searched `tools/PcTools/src` for any other code reasoning about `idle`
never following `running`/`paused`. Found none:

- `log_analysis.py` (`_default_run_index`, `select_run`) already assumes
  the *opposite*, correctly -- its docstring explicitly anticipates "a
  trailing idle tail... after a firing finishes **or aborts**" and treats
  a captured idle segment as a normal, expected artifact to skip past, not
  as something that can't happen.
- `devices_profiles.py` and `log_analysis.py` both have an `idle`/state-0
  name table (`STATE_NAMES` / `_PROFILE_EXEC_STATE_NAMES`), but neither
  reasons about *transitions*, only naming.
- No other file in `tools/PcTools/src` contains a "never transitions" /
  "only to DONE or FAULTED" style claim about `profile_exec` state.

So the false assumption is confined to `run_queue.py`'s own docstring and
this one behavioral gap; nothing else needs the same fix.

## `is_rested()` skips invalid channels -- separate issue, not fixed here

`run_queue.is_rested()` (lines 124-149) ignores any channel reporting
`valid: false` rather than treating it as "not rested". The docstring
states the trade-off deliberately: "a dead channel should not block every
other zone's queue forever, and the ceiling/fault checks below catch a
genuinely unsafe start on their own terms."

**Recommendation: tighten this, don't leave it as-is.** The stated
mitigation is weaker than it sounds for exactly the scenario that matters
most here: a thermocouple going invalid (`NaN`, per
`project_saftyfw_tc_invalid_is_one_cr1_byte` /
`project_iae_noise_floor_unknown` -- this repo has already hit this exact
fault mode) on the ONE channel that is still hot from a prior firing.
`check_targets_within_ceiling` only compares the *profile's peak target*
against each zone's configured `max_temp_c` -- it has no way to see that a
zone is currently hot; that is exactly what `is_rested`/`wait_until_rested`
exist to catch, and it is exactly the check a faulted channel silently
falls out of. `check_no_fault`/`check_not_faulted` run during the poll
loop *after* the run has already started, so they cannot prevent the
start itself. Net effect: a hot zone whose thermocouple faults at the
wrong moment can pass every pre-start safety check and get a fresh preset
+ profile fired into it while still hot.

A better trade-off that keeps the "one dead channel shouldn't wedge the
whole queue forever" goal: refuse to start (raise `RunQueueError`, same
posture as every other pre-start check in this module) when a channel is
`invalid` **and** there is no other, independent signal that the zone it
belongs to is actually cold -- e.g. require an operator override flag or a
recent-good reading on record, rather than silently proceeding as if the
channel had never existed. At minimum, log a loud warning naming the
invalid channel(s) every time `is_rested` returns `True` with any channel
excluded, so an unattended campaign's log makes this visible even if the
behavior isn't changed immediately. This was not touched in this patch --
flagging it for a deliberate follow-up decision, not silently fixing a
second thing inside a patch framed as the idle-stop fix.
