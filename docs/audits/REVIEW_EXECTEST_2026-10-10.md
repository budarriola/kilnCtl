# Review: execfx2 test gaps (`8e2993867`) -- 2026-10-10

Adversarial review of `8e2993867` on origin/dev. It closes LOW-1..3 of
`REVIEW_EXECFX2_2026-10-10.md`:

- a PSRAM-only failure mode in the `heap_caps_malloc` stub, and a test that a
  start from DONE refuses when the PSRAM snapshot cannot be allocated;
- a one-shot `xSemaphoreTake` hook and `test_epoch_compared_under_lock`;
- epoch sample-point tests for the aux and autotune relay paths;
- `io_seg_start_since(idx, seg, off_epoch)`, so the warm replay samples the
  all-off epoch once, before the LD-01 recheck.

Summary: 0 HIGH, 0 MED, 1 LOW, INFO notes. The production change is correct.
All five mutations below were CAUGHT, so each new test detects the regression
it was written for.

## Production change: `io_seg_start_since()`

- **Callers.** `io_seg_start()` has one production caller, the tick path
  (`profile_executor.c:1049`). It still samples inside `io_seg_start()`, just
  before the post, as it did before this commit. The warm replay
  (`profile_executor_run.c:1440-1473`) is now the only caller of
  `io_seg_start_since()`. Behaviour of the tick path is unchanged.
- **Warm replay sample point.** `warm_off_epoch` is read after every other
  late refusal (backup, danger mode, factory reset, zones generation) and
  immediately before `relay_authority_start_blocked()`. Only that recheck
  runs between the sample and the replay loop. An all-off during the recheck
  now drops every replayed ON, which closes the gap REVIEW_EXECFX2 LOW-3
  described.
- **Can a stale sample refuse a legitimate ON forever?** No. The owner
  compares under the kiln_io lock (`kiln_io_set_relay_mask_if_epoch()`), and
  the epoch moves only in `kiln_io_all_relays_off()`. A drop therefore needs
  a real all-off after the sample. Every all-off caller is a fail-safe or an
  explicit operator all-off (guard-9/stale-tick, danger mode, sweep, safety
  ceiling sync, relay-unknown watchdog, PC `ALL_RELAYS_OFF`). Dropping the ON
  is the designed LOW-3 semantics in each case.
  - Using one sample for the whole loop means an all-off that lands between
    two replayed segments drops the later ones too. Before this commit, each
    segment sampled again, so a later segment could still close a relay after
    that all-off. The new behaviour is the stricter and correct one: the all-off
    came after the start decision.
  - A dropped replayed ON is not retried. The segment is forced
    `blocking = true`, `io_segs_tick()` skips it, and `state_on` stays true
    while the relay is off. That is the same as any dropped `io_seg_start()`
    ON before this commit. It is logged with `LOGW` ("write failed ... state is
    unknown"). See INFO-2.
  - The sample is taken even when `warm_replay_end == 0`. That is harmless.
- **Can it wrongly allow an ON after an all-off?** No. Any all-off after the
  sample makes the queued ON stale. Any all-off before the sample was already
  before the old (later) sample point, so nothing is newly allowed. A
  persistent cause is still caught by the LD-01 recheck.
- **Merge with `b76d03657`.** That commit is an ancestor. The recheck is the
  err_msg form, `relay_authority_start_blocked(s_exec.safety, err_msg,
  err_cap, "a firing cannot start")`, and its refusal unwinds both claims and
  calls `run_refuse_unlock(done_snap)` before any replay. A refused start
  still writes no relay.

## Stub hooks

- `heap_caps_malloc_test_set_fail_spiram()`: the new test sets it and clears
  it. `TEST_CHECK` does not abort, so the clear always runs. The flag is a
  per-TU `static`, the same pattern as the existing `_set_fail`. It works
  because the host tests `#include` the production `.c` into the test TU.
- `g_test_stub_semaphore_take_hook`: a `selectany` global, cleared before it
  is invoked (so the hook cannot recurse into itself) and cleared again by
  the test afterward, so a take that never happens cannot leak it.
- The epoch bump flags (`s_test_bump_epoch_in_aux_gate`,
  `s_test_bump_epoch_in_link`, `g_at_bump_epoch_in_gate`) are all set and
  cleared around a single call. The fake epochs stay incremented, but every
  stub compares against the current value, so that is harmless.

## Do the tests prove what they claim?

Yes. Each one was mutated (table below), and each CAUGHT names the new test's
own assertion.

- The PSRAM test fails on a restored internal allocation, because the run
  then succeeds. It is the first SPIRAM request on that path that can refuse:
  `history_buf_ensure_alloc()` also requests SPIRAM, but failing it only logs.
- The under-lock test fails when the comparison is moved in front of
  `kiln_io_lock()`.
- The warm replay test bumps the epoch on every `safety_link_get_status()`
  call. It therefore proves "sampled before the recheck", not "sampled
  immediately before the recheck". A sample taken even earlier would also
  pass. That would only widen the drop window, so it is not unsafe, and no
  extra test is needed.

## LOW

### LOW-1: two new tests leave executor or stub state behind (FIXED in 75eee94b2)

- `test_done_start_psram_alloc_failure_refuses_without_internal_fallback`
  leaves `s_exec.state == PROFILE_EXEC_DONE` and a fresh `s_exec.lock`
  mutex. The sibling MED-2 test resets `s_exec.state = PROFILE_EXEC_IDLE` at
  its end; this one does not. The on/off tests that follow pass today because
  they arrange their own state.
- `test_epoch_compared_under_lock` leaves `g_test_stub_semaphore_take_default
  = 1`, `s_dispatch_io.initialized = true` and `s_dispatch_io.exp` pointing at
  a dummy. It is the last test in `main()`, so nothing inherits that state
  today. A test appended after it would.
- Neither causes a failure now. Both create a test-order dependence.
- Fix: restore `s_exec.state = PROFILE_EXEC_IDLE` in the first test, and in
  the second restore `take_default`, `initialized` and `exp` to their
  previous values.

## INFO

- **INFO-1 (FIXED in 75eee94b2: a gate was needed, link fault sources can rise mid-run; io_seg_start_since now checks relay_authority_on_blocked, test + negtest CAUGHT): the tick path has no gate in front of `io_seg_start()`.**
  `apply_relay()`, `aux_apply_relay()` and autotune each check relay_authority
  between their sample and their post. The tick's relay-IO ON
  (`profile_executor.c:1049`) checks no relay_authority source, and the owner's
  AUTHORIZED path is ungated by design. Its sample sits inside
  `io_seg_start()`, after the tick's state decisions. This is unchanged by
  this commit, and REVIEW_EXECFX2 already noted that `io_seg_start()` has no
  gate of its own. A fail-safe all-off before that sample is caught only if
  the run has already left RUNNING. I did not check whether any
  relay_authority source (updating, crash_unack, mode gate) can come up
  mid-run without faulting the executor. That is worth a follow-up look.
- **INFO-2: a dropped replayed ON stays dropped for the rest of the run.**
  The start still reports success. The test checks this explicitly ("the
  start itself still succeeds"). Only the `LOGW` shows it. This is
  consistent with the design (never close a relay after an all-off). An
  operator who wants the relay ON again has to restart the run.

## Tests run

- `firmware\KilnFW\App\test\build_host_tests.ps1 -Only
  "^(profile_executor_prestart|kiln_io_owner|autotune_engine_prestart)( |$)"`
  at `8e2993867`: all 3 executables built and passed (5946/5946, 914/914,
  101/101).

## Negative tests

Command: `tools\negtest.ps1 -Command "build_host_tests.ps1 -OutDir '{OUT}'
-Only '^(profile_executor_prestart|kiln_io_owner|autotune_engine_prestart)( |$)'"
-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
-Parallel 2`, base `8e2993867`. Baseline passed (20.7 s), real tree
unchanged, copies removed. Verdict `ALL_CAUGHT`.

| # | Mutation | Verdict | Failing assertion |
|---|----------|---------|-------------------|
| 1 | Warm replay back to `io_seg_start()` (re-sample after the LD-01 recheck) | CAUGHT | `test_profile_executor_prestart.c:11104` |
| 2 | DONE snapshot allocated with `MALLOC_CAP_8BIT` only (internal RAM) | CAUGHT | `test_profile_executor_prestart.c:11116-11119`, `3308-3314` |
| 3 | `kiln_io_set_relay_mask_if_epoch()` reads the epoch before `kiln_io_lock()` | CAUGHT | `test_kiln_io_owner.c:771-772` |
| 4 | `autotune_apply_relay()` posts with a fresh epoch (sampled after the gate) | CAUGHT | `test_autotune_engine_prestart.c:7384` |
| 5 | `aux_apply_relay()` posts with a fresh epoch (sampled after the gate) | CAUGHT | `test_profile_executor_prestart.c:11080` |

In REVIEW_EXECFX2's table, mutations 2 and 5 were MISSED, and 3 and 4 were
not mutated because no test could catch them. All four are CAUGHT now.
