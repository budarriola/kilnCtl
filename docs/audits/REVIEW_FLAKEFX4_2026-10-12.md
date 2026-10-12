# Review: flakefx4 (aba482d76) -- update_fetch wedge waits and idle-wait handshake

Date: 2026-10-12. Reviewer: Claude (opus), adversarial review, no subagents.
Scope: commit `aba482d76` on origin/dev, which fixes `REVIEW_FLAKEFX3_2026-10-12.md`
LOW-1/LOW-2 and `REVIEW_SAFTYFX6_2026-10-12.md` LOW-1:

- `firmware/KilnFW/App/test/test_update_fetch.c`: `WAIT_FOR_MS(cond, ms)` (147), used
  with a 30 s ceiling for the two positive wedge waits (465, 467); `Sleep(50)` replaced
  by a wait on `g_fr_sem_idle_waiters >= 1` plus a CHECK (521-522) before the
  `g_fr_sem_bad_n == 0` check (523).
- `firmware/KilnFW/App/test/stubs_update_fetch/fake_support.{c,h}`: `g_fr_sem_idle_waiters`,
  incremented and decremented (Interlocked) around the `WaitForSingleObject` of every
  `portMAX_DELAY` `xSemaphoreTake`.
- `firmware/SaftyFW/src/safety_guards.c`: comment-only rewording of gap (1) in
  `s8_clear_refuse()` (191-193).

Reviewer worktree base: `3ad318ba3` (origin/dev tip at the time, contains `aba482d76`).

## Verdict

No HIGH or MED findings. The handshake is sound: it proves the released writer's final
`xSemaphoreGive(wr_done)` happened before the check. The 30 s waits fail loudly through
the CHECK that follows each one, and do not hang. One LOW (a stale comment line) and six
INFO notes.

## Questions asked

1. **Does the handshake prove the writer's last give happened before the check?** Yes.
   - In `wr_task()` (`update_fetch.c:353-400`), the last semaphore call of an abandoned
     op is `xSemaphoreGive(s_c->wr_done)`. The loop then returns to
     `xSemaphoreTake(s_c->wr_req, portMAX_DELAY)`. That take is the writer's only unbounded
     take: `update_stage.c` takes no semaphore, and `st_lock()` is not called by the writer.
   - In the fake (`fake_support.c:321-331`), `InterlockedIncrement` runs after
     `sem_is_bad()` and before `WaitForSingleObject`. The increment is program-ordered
     after the give on the same thread, and `InterlockedIncrement` is a full barrier. Seeing
     `g_fr_sem_idle_waiters >= 1` therefore implies the give, and any `g_fr_sem_bad_n`
     increment it made, are visible.
   - **Could another task be the idle waiter?** Not today. `g_fr_live_tasks <= 1` is checked
     at 488 and 493, and nothing creates a task after that. The main test thread never
     blocks in an unbounded take during the wait, and the fake's flash block
     (`maybe_block()`, `fake_support.c:581-588`) waits on a raw event, which is not counted.
     The wait cannot be satisfied early either: before `fr_flash_release()` the writer is in
     `maybe_block()`, not counted. See INFO-1 for the future-regression case.
   - **Data race on the counter?** None. Writers use `InterlockedIncrement/Decrement`. The
     reader is a `volatile long` load, which is atomic for an aligned 32-bit value on x86/x64.
     MSVC's default `/volatile:ms` gives it acquire semantics.
   - If `wr_stop()` had deleted `wr_req`, `sem_is_bad()` returns before the increment. The
     writer then never counts, so the wait times out and 522 and 523 fail, or `ExitProcess`
     fires after 5000 bad calls. Both outcomes are loud.
2. **Does WAIT_FOR_MS fail loudly on timeout?** Yes, indirectly. The macro itself only
   loops (bounded by `GetTickCount()` deltas, wrap-safe in DWORD arithmetic). Every use is
   followed by a CHECK of the same condition (466, 468, 522). Mutants B1, B2 and A4 below
   time out and fail at those CHECKs. The run ends in 99 s to 189 s, well under the 300 s
   `TestTimeoutSec`. Nothing hangs.
3. **Any test now vacuous?** No. 523 still catches every `wr_stop` wedge mutant (A2, A5),
   and the handshake turns the delayed-give case from MISSED into CAUGHT (A5 vs A6).

## Negative tests

Command: `tools\negtest.ps1 -Command 'powershell ... build_host_tests.ps1 -OutDir "{OUT}"
-Only update_fetch' -Mutations <scratch>\mut.json -ExpectPattern
"(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`, `-Parallel 3` (run 1) and
`-Parallel 2` (run 2).

- Base `3ad318ba3`. Both baselines passed. Both runs reported `real_tree_unchanged: true`
  and `copies_removed: true`.
- Every mutant log shows `Built: 3/3 executables` and zero `BUILD FAILURES` or compiler
  `error` lines. Every CAUGHT is a failing CHECK, never a compile failure. `-Only update_fetch`
  selects `test_update_fetch`, `test_update_http_refusals` (which includes the same file) and
  `main`. The wedge mutants fail in both of the first two.

| Id | Mutation | Verdict | Failing assertion(s) |
|---|---|---|---|
| A1 | test: handshake removed (`WAIT_FOR_MS(idle_waiters)` deleted, its CHECK made `CHECK(1, ...)`) | MISSED (expected) | none -- see below |
| A2 | `update_fetch.c` `wr_stop()`: on a wedge, delete and NULL `wr_done` (one bad give, after stage IDLE) | CAUGHT | :523 |
| A3 | A2 + handshake removed | CAUGHT | :523 (normal load: the give lands before the check anyway) |
| A4 | fake: increment `g_fr_sem_idle_waiters` after `WaitForSingleObject` (parked writer never counted) | CAUGHT | :522 after the 30 s wait |
| A5 | A2 + fake `xSemaphoreGive(NULL)` sleeps 300 ms first (writer descheduled before its bad give) | CAUGHT | :523 |
| A6 | A5 + handshake removed | MISSED (expected: shows the handshake is load-bearing) | none |
| B1 | `update_fetch_writer_wedged()` returns false | CAUGHT | :466 after 30 s, also :499, :503, :517 |
| B2 | timeout path sets `wr_wedged = false` (wedge never latched) | CAUGHT | :466, :469, :485, :499, :503, :515, :517, :522, :523 |

**On A1 (requested mutation (a), "check runs before the writer parks").** The test cannot
catch this mutation on its own, and no test could. The handshake orders the test's own
observation. With correct product code there is no bad call to observe, early or late, so
removing the handshake changes no outcome. What the handshake protects is the detection of
a give-side bug, as in FLAKEFX3 LOW-2. A5 and A6 test exactly that. With the bug present
and the writer's bad give delayed 300 ms, the handshake version fails at 523 (A5). The
version without the handshake, which models the old `Sleep(50)` and anything shorter than
the delay, passes (A6). At normal load the delay is microseconds, so even the
handshake-less version catches it (A3).

## Findings

### LOW-1: stale comment line contradicts the handshake

- **File:line:** `test_update_fetch.c:518`.
- The line reads "the released writer must have exited before the semaphore check (no fixed
  sleep)". It is a leftover from an earlier draft. The writer never exits: `wr_stop()` skips
  WR_EXIT on a wedge. The two lines that follow (519-520) correctly say it is back parked in
  its idle wait.
- **Failure scenario:** a maintainer who believes the writer exits could "fix" the wait to
  `g_fr_live_tasks == 0`, which never becomes true. The test would then fail after 30 s on
  correct code, or be weakened to make it pass.
- **Fix:** delete line 518.

### INFO-1: the counter is global, not per-semaphore or per-task

- **File:line:** `fake_support.c:253, 324-330`; `test_update_fetch.c:521`.
- The handshake counts any thread blocked in any unbounded take. It is sound today only
  because the writer is the sole live task after 488/493. Suppose a future regression left a
  second thread parked unbounded, for example a fetch task deadlocked in `st_lock()`. That
  thread would satisfy `>= 1` before the writer's give, and the handshake would silently
  revert to "no wait".
- Also, `g_fr_sem_idle_waiters` is not reset in `fr_reset()`. It is balanced by
  construction, so a leaked waiter from an earlier test would only make it start above 0.
- **Optional hardening:** snapshot the count right after `fr_flash_release()` and wait for
  `snapshot + 1`. Or record the semaphore handle of each unbounded waiter and wait for one on
  `wr_req`.

### INFO-2: the 30 s ceiling does not bound the fake's iteration-counted fallback

- **File:line:** `fake_support.c:315` (`++waited > 15000`), `test_update_fetch.c:465`.
- FLAKEFX3 LOW-1's first fix option was taken (a longer bound), not the second (a wall-clock
  fallback). If the writer has not reached `maybe_block()` within 30 s, the scaled clock
  still starts only after 15000 poll iterations, and each iteration takes far more than 1 ms
  under starvation. The flake is much rarer now, but not impossible on an extremely starved
  runner. A `GetTickCount()`-based fallback would close it. This is not a regression.

### INFO-3: WAIT_FOR_MS shares `g_t0` with WAIT_FOR

- **File:line:** `test_update_fetch.c:143-147`.
- Both macros reuse the one global `g_t0`. They are never nested and run on the main thread
  only, so this is harmless.

### INFO-4: worst-case run time on a broken build grows

- B2 took 189 s, against 50 s for the baseline. Several 30 s and 5 s waits expire one after
  another. This is still under the 300 s `TestTimeoutSec`. A future third 30 s wait in this
  case should check the budget.

### INFO-5: SaftyFW gap (1) comment is now accurate

- **File:line:** `safety_guards.c:189-194`.
- The new text: the clear is granted "once the burst has left the partial window, or earlier
  under (2) and (3)", and the partial window refuses only a burst it still sees "after the
  first S8_POST_MIN_S". This matches the code: the partial check is gated on
  `s8_post_elapsed_s >= S8_POST_MIN_S`, and its rate uses elapsed time. It resolves SAFTYFX6
  LOW-1.
- The diff touches only ` *` comment lines, so code behavior is unchanged. SAFTYFX6 INFO-1
  (gap (2) omits the streak refusal) is untouched, as it was optional.

### INFO-6: no negtest by the fixer

- The fixer's commit ran no negative test. The table above supplies it: 6 CAUGHT, 2 MISSED,
  and both MISSED results are the expected outcome.

## Fix status

| Finding | Status |
|---|---|
| LOW-1 | FIXED (stale line removed; the following comment already says the writer parks) |
| INFO-1 | FIXED (counter reset in `fr_reset()`; snapshot-based hardening not done) |
| INFO-2 | FIXED (fallback is wall-clock `GetTickCount64()`, 15 s, inside the 30 s ceiling) |
| INFO-3..6 | no action required |
