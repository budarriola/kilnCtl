# Review: execfx2 (fixes for REVIEW_EXECFX MED-A, LOW-B..E) -- 2026-10-10

Scope: `93336ac79`, `ee2b0b7ad`, `7c5cfaa18`, `9c9600384` on origin/dev, which
fix `docs/audits/REVIEW_EXECFX_2026-10-10.md`. Also checked: the rebase merge of
`kiln_io.h` and `test_kiln_io_owner.c` with dk7fix `235e4e3c4`
(`KILN_IO_ERR_UNSERIALISED_OFF`, 0x10C). Adversarial review, no board access,
no code changes. Line numbers refer to `9c9600384`.

Summary: 0 HIGH, 0 MED, 3 LOW, INFO notes. Every finding is fixed as
described. No path found where a refused start energizes or claims a relay.
The LOW-D lock placement is correct. Two of the fixes have no test that
would catch a regression: LOW-C and LOW-D's under-lock placement. LOW-E is
tested only through `apply_relay()`; see the negtest table.

## Findings checked

### MED-A (warm-start replay after the last refusal): FIXED, correct

- The replay loop moved from the warm-start planning block to
  `profile_executor_run.c:1452`. It now runs after the LD-01 start-gate
  recheck (`:1440`), which is the last `return false` in the function.
- Between `:1452` and the RUNNING commit (`:1585`) these run, and none of
  them can refuse: the replay loop, latch clear, aux handoff and
  `relay_authority_claim_mask`.
- Nothing between the old and the new location reads `io_segs[]`,
  `claimed_relay_mask` or `warm_start_replayed_*`. The move does not change
  any input of the code it passes.
- The fix also covers the pre-existing IDLE-start orphan (no snapshot on that
  path). The IDLE case is what `test_med_a_refused_warm_start_leaves_relay_off_and_unclaimed`
  tests, at all 11 refusal sites.
- Side fix in the same commit: the LD-01 site used a bare
  `xSemaphoreGive()`, so a refusal there from DONE leaked `done_snap` and
  skipped the restore. It now calls `run_refuse_unlock(done_snap)`. The
  "LD-01 link" row of the MED-2 table covers it.
- Ordering relative to the aux handoff is unchanged: the replay still runs
  before the handoff OFF write. Overlap between a RELAY_IO target and an
  enabled aux is refused at `:587`.

### LOW-B (history kept across a refused start): FIXED, correct

- `run_refuse_unlock()` keeps the live `s_exec.history` pointer when it is
  not NULL.
- `history_buf_ensure_alloc()` is idempotent, so the live pointer is either
  the snapshot's own pointer or a buffer this start allocated.
- No sample is written before the commit. The restored
  `history_count`/`history_head` therefore still describe intact buffer
  contents, and the DONE graph survives the refusal.

### LOW-C (snapshot PSRAM only): FIXED

- The internal-RAM fallback is gone. If the PSRAM allocation fails, the
  start is refused with "out of memory preparing the start -- try again".
- That refusal comes before any `s_exec` mutation and before any
  relay_authority claim. The zone claim is taken at `:1308`, the heat claim
  at `:1340`.

**Is the PSRAM-only refusal acceptable? Yes.**

- `sdkconfig.defaults` sets `CONFIG_SPIRAM=y` and `CONFIG_SPIRAM_USE_MALLOC=y`,
  and does not set `CONFIG_SPIRAM_IGNORE_NOTFOUND`. A board without working
  PSRAM does not boot this image at all.
- The request is 3320 B (`sizeof(s_exec)`) out of 8 MB of PSRAM. It fails
  only when PSRAM is exhausted or badly fragmented, and at that point LVGL,
  the history buffer and the persist scratch allocations are failing too.
- It only affects a start from DONE. A start from IDLE takes no snapshot,
  so dismissing the finished run is a working fallback for the operator.
- It fails closed, says why, and changes nothing.
- Untested: see LOW-1.

### LOW-D (epoch compared under the kiln_io lock): FIXED, correct

- `kiln_io_set_relay_mask_if_epoch()` (`kiln_io.c:824`) takes the kiln_io
  lock, then compares `since_epoch` with `s_relay_off_epoch`, then writes.
  `kiln_io_all_relays_off()` still bumps the epoch before it tries the lock.
- If the owner reads the old epoch while holding the lock, the all-off has
  not taken the lock yet. Its locked OFF write therefore lands after the
  owner's write, and OFF wins.
- If the all-off held the lock earlier, its unlock (release) followed its
  bump, and the owner's lock (acquire) makes the new epoch visible, so the
  write counts as stale.
- The window from REVIEW_EXECFX is closed for the locked all-off.
- The unlocked fallback is still the documented K7 class. With dk7fix it
  now returns `KILN_IO_ERR_UNSERIALISED_OFF`, so the off-tracker does not
  record it and the watchdog retries.

### LOW-E (callers sample the epoch before their gate): FIXED

- `apply_relay()`, `aux_apply_relay()`, `io_seg_start()` and autotune's
  `autotune_apply_relay()` sample `kiln_io_relay_off_epoch()` before their
  gate. They post through `kiln_io_owner_command_set_relay_mask_authorized_since()`.
- Every remaining caller of the epoch-less `_authorized()` posts value 0
  (OFF), so the epoch does not matter for any of them. Checked with a grep
  of non-test `.c` files.
- The stale-OFF semantics are unchanged: OFF bits are still written, and
  the call returns `ESP_ERR_INVALID_STATE` only when an ON was dropped.

### dk7fix merge (`235e4e3c4` + `93336ac79`): coherent

- `kiln_io.h` declares both `KILN_IO_ERR_UNSERIALISED_OFF` and the new helper.
- `kiln_io_all_relays_off()` keeps the bump as its first statement, ahead of
  the NULL/expander check. The test checks "epoch bumped although the all-off
  failed".
- The unserialised path returns 0x10C, never `ESP_OK`.
- The owner's `CMD_ALL_RELAYS_OFF` records the off-tracker only on `ESP_OK`.
- `test_kiln_io_owner.c` keeps dk7fix's N8 test
  (`test_owner_task_sx_reset_notes_off_tracker`) and adds
  `test_epoch_stamp_and_stale_semantics` after it. Both run from `main()`.
- No conflict residue.

## LOW

### LOW-1: LOW-C has no test; a restored internal fallback goes unnoticed

- The host `heap_caps_malloc` stub (`stubs/esp_heap_caps.h`) can only fail
  every allocation. It cannot fail SPIRAM while leaving internal RAM
  working, so no test can tell the fixed code from the old fallback.
- Negtest: re-adding the fallback is MISSED (table).
- Fix, either of:
  - add a caps-aware failure switch to the stub (fail only when
    `MALLOC_CAP_SPIRAM` is requested), with a DONE-start test that expects
    the refusal and an unchanged `s_exec`;
  - add a source-level check that the snapshot allocation names only
    `MALLOC_CAP_SPIRAM`.

### LOW-2: LOW-D's under-lock placement is not tested

- `test_epoch_stamp_and_stale_semantics` covers the helper's result only:
  stale is reported and no relay is driven ON.
- Moving the comparison in front of `kiln_io_lock()`, or back into the owner
  task (the original TOCTOU), would still pass.
- Fix: a test that bumps the epoch from inside the stub
  semaphore-take (an all-off arriving while the owner waits for the lock)
  and expects the stale drop.

### LOW-3: LOW-E is tested only through `apply_relay()`

- `test_apply_relay_samples_epoch_before_gate` bumps the epoch inside the
  `relay_authority_zone_blocked()` stub, which covers `apply_relay()`.
- The aux path has no matching test. Its gate stub,
  `relay_authority_on_blocked()`, never bumps the epoch, so the negtest
  mutation that samples after the gate is MISSED.
- The autotune host test stubs `_since()` as "ignore since", so its sample
  point is not exercised at all.
- `io_seg_start()` has no gate of its own, and its sample point cannot be
  told apart from a post-time sample.
- For the warm replay, the real decision is the LD-01 recheck (`:1440`).
  The replay samples the epoch inside `io_seg_start()`, after that recheck.
  An all-off between the two is not caught, and the replayed ON goes out.
  The window is narrow, and the next tick does not re-command a replayed
  (blocking) segment.
  - INFO-grade, but worth noting: sampling once before the LD-01 recheck
    and passing it into the replay would close it.

## INFO

- **A refused start never energizes or claims a relay.** The checks:
  - Each of the 11 late sites, and the cap/no-rule sites, returns before
    the replay, aux handoff, `relay_authority_claim_mask` and
    `heat_enable_acquire_since`.
  - Refusals from DONE restore the snapshot. The snapshot holds
    `claimed_relay_mask`/`io_segs`, which were not mutated toward ON anyway.
  - `clear_stale_zone_latches_for_new_run()` is still after the last
    refusal.
- **Ambiguous ESP_ERR_TIMEOUT path.** In
  `kiln_io_set_relay_mask_if_epoch()`, a lock timeout returns before the
  epoch comparison. The owner then logs no "stale dropped" line, even if the
  command was stale. The write did not happen, so this is only a logging
  difference.
- **Fail-safe all-off cadence.** If the relay-unknown watchdog keeps retrying
  `kiln_io_all_relays_off()` while `relay_state_unknown` is raised, every ON
  posted in that period is dropped. That is the intended behavior.
  - The remaining all-off callers are guard-9/stale-tick force-offs,
    danger-mode, sweep, safety ceiling sync and the PC `ALL_RELAYS_OFF`
    command. None of them runs on a normal heating tick, so LOW-E adds no
    false refusals in steady state.

## Tests run

- `build_host_tests.ps1 -Only "profile_executor|kiln_io|autotune"` at
  `9c9600384`: 13/13 executables built and passed.

## Negative tests

Command: `tools\negtest.ps1 -Command "build_host_tests.ps1 -Only
'test_profile_executor_prestart|test_kiln_io_owner' -OutDir {OUT}"
-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
-Parallel 3`, base `9c9600384`.

Baseline passed (33.7 s). Real tree unchanged; copies removed.

| # | Mutation | Verdict | Caught by |
|---|----------|---------|-----------|
| 1 | MED-A: replay loop moved back before the late refusals | CAUGHT | `test_profile_executor_prestart.c:4211-4213` (ON write reached hardware, segment registered, PROFILE claim left behind) |
| 2 | LD-01 site: bare `xSemaphoreGive()` instead of `run_refuse_unlock(done_snap)` | CAUGHT | `test_profile_executor_prestart.c:3305-3310` (DONE snapshot not restored) |
| 3 | LOW-B: restore drops the live history pointer | CAUGHT | `test_profile_executor_prestart.c:3389` |
| 4 | LOW-C: internal-RAM snapshot fallback re-added | MISSED | none (LOW-1) |
| 5 | LOW-D: `kiln_io_set_relay_mask_if_epoch()` never reports stale | CAUGHT | `test_kiln_io_owner.c:732-733, 796-797` |
| 6 | LOW-E: `apply_relay()` samples the epoch after its gate | CAUGHT | `test_profile_executor_prestart.c:7436` |
| 7 | LOW-E: `aux_apply_relay()` samples the epoch after its gate | MISSED | none (LOW-3) |

The two MISSED rows are the expected ones and are filed as LOW-1 and LOW-3.
LOW-D's under-lock placement (LOW-2) and the autotune sample point were not
mutated: by inspection no current test can tell them apart (see LOW-2, LOW-3).
