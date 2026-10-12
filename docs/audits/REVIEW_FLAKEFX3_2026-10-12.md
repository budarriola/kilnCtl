# Review: flakefx3 (bd18871ef) -- update_fetch wedge test hardening

Date: 2026-10-12. Reviewer: Claude (opus), adversarial review, no subagents.
Scope: commit `bd18871ef` on origin/dev, the test-only fix for
`REVIEW_FLAKEFX2_2026-10-12.md` LOW-1..3 and INFO-1/2:

- `firmware/KilnFW/App/test/test_update_fetch.c`: `g_fr_live_tasks <= 1` checks
  after a refused download and a check (485, 490), a `bytes_total:20000` check
  that the wedge is hit after WR_BEGIN (467), and a `g_fr_sem_bad_n == 0` check
  after a 50 ms settle (515-516).
- `firmware/KilnFW/App/test/stubs_update_fetch/fake_support.c`: deleted-semaphore
  tracking (`s_deleted[64]`, 251-262, 332-344) and `sem_is_bad()` (266-281). A
  take or give on a NULL or deleted handle is counted, sleeps 1 ms, returns
  pdFALSE, and calls ExitProcess(1) after 5000 hits.
- `#include "update_http_internal.h"` replaces a hand-written forward
  declaration.

## Verdict

No HIGH or MED findings. Every new check is load-bearing (each named guard
mutant is caught by an assertion in the new lines). The commit introduces no
new flake. Under heavy starvation one pre-existing flake remains, at line 463
(LOW-1). It reproduces at the same rate on the parent commit.

## Evidence

### Full host suite at the dev tip

`build_host_tests.ps1` on `bd18871ef`: 98/98 executables built and passed. The
negtest baseline repeated this: 98/98 passed.

### Negative tests

Command: `tools\negtest.ps1 -Preset kilnfw-host -Mutations <json> -ExpectPattern
"(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 4`.

Verdict: ALL_CAUGHT. The baseline passed and the real tree was unchanged. For
every mutation I checked the log: the mutant compiled, and a test assertion in
`test_update_fetch.exe` (and in its `test_update_http_refusals.exe` include
twin) failed.

The M2 and M4 logs also show one `BUILD FAILURES (1)` entry. These are
unrelated parallel permission-denied errors on shared `.obj` and build-gate
ticket files. In both runs the target executables built, ran and failed on
the assertions listed below.

| Mutation | Change in `drivers/update/update_fetch.c` | Caught at |
|---|---|---|
| M1 | wr_call wedge guard removed (`if (0 && s_c->wr_wedged)`) | test:505 (wr_call guard counter) |
| M2 | `wr.cmd` written before the wr_call guard | test:512 (stage not back to idle) |
| M3 | wr_stop ignores the wedge | test:516 (`g_fr_sem_bad_n`) |
| M4 | wr_start ignores the wedge | test:485 and test:490 (`g_fr_live_tasks`) |
| M6 (new) | wr_start creates both semaphores, then `goto fail` when wedged | test:516 (`g_fr_sem_bad_n`) |
| M7 (new) | `stage_begun = false` in stage_begin | many, including refusals:273/278/290 and update_fetch:221/235/253/463/468 |
| M8 (new) | wr_stop on a wedge deletes and NULLs only `wr_done` | test:516 (`g_fr_sem_bad_n`) |

M6 and M8 show that the deleted-handle tracking catches partial-cleanup bugs,
not only the full delete in M3.

### Stress runs

The machine ran at about 100% CPU from concurrent negtest builds. Each batch
started 30 or 60 instances of the executable at once.

- Normal priority, `bd18871ef`: `test_update_fetch.exe` passed 60/60, and
  `test_update_http_refusals.exe` passed 60/60.
- Idle priority, `bd18871ef`: 7/30, 18/60, 8/30 and 9/30 failed. Every failure
  was at test:463.
- Idle priority, parent `2b185a99a`: 4/60, 6/30 and 7/30 failed at test:463,
  plus one at test:480.
- None of the 300+ runs failed at a line added by this commit (467, 485, 490
  or 516).
- M3 mutant at Idle priority: caught 60/60 at test:516. Instrumentation showed
  that `g_fr_sem_bad_n` was already 3 before the `Sleep(50)`.
- Instrumented build: each run recorded 16-18 deletes, used at most 7-12 of
  the 64 slots, and dropped none.

## Findings

### LOW-1 (pre-existing, not introduced): the wedge wait at test:462 is bounded at 5 s, but the fake's clock fallback can take minutes

- **File:line:** `test_update_fetch.c:144` (`WAIT_FOR` gives up after 5000 ms),
  `:462-463`, and `stubs_update_fetch/fake_support.c:314` (`++waited > 15000`
  counts 1 ms poll iterations).
- **Failure scenario:** On a starved machine (Idle priority under a parallel
  build), the writer thread does not reach the blocking flash write within 5 s.
  The scaled clock starts only once `g_fr_flash_blocked_n >= 1`, or after
  15000 poll iterations. Each poll is a `WaitForSingleObject(…, 1)`, which
  under starvation takes far longer than 1 ms, so the fallback can take
  minutes. The positive wait at 462 expires first, and 463 fails with "a
  timed-out writer op marks the writer wedged". The rates match on the parent
  commit, and the normal-priority suite is clean, so this does not block. It
  is the most likely source of an occasional red `check_00_kilnfw_host_tests`
  on a loaded runner.
- **Fix:** Give positive waits that depend on writer start-up a longer bound,
  for example a `WAIT_FOR_MS(cond, 30000)` variant used at 462/464. Negative
  waits do not need it. Alternatively, make the fake's fallback wall-clock
  based (`GetTickCount()` delta) instead of counting iterations.

### LOW-2: the `Sleep(50)` at test:515 makes give-side-only detection timing-dependent

- **File:line:** `test_update_fetch.c:515-516`.
- **Failure scenario:** A future regression could make the released writer do
  exactly one bad call, such as a single `xSemaphoreGive(wr_done)` on a
  deleted handle, and do it after the check has run. Normally the writer has
  already finished by 512 (stage idle), so this is unlikely. But the window
  between `stage_phase() == IDLE` (set inside the writer's abort) and the
  writer's final give is not ordered with respect to the check. Under
  starvation, 50 ms may not cover it, and the check passes. This can only
  produce a false negative, never a false failure. The take-side loop in M3
  shows up long before 515: the count was already 3 before the sleep.
- **Fix:** Wait for an event instead of a fixed time. The fake could count the
  writer task's exit (`g_fr_live_tasks` dropping to 0 for the parked writer),
  with `WAIT_FOR(g_fr_live_tasks == 0)` before the check. Alternatively, count
  gives on `wr_done` and wait for that count to advance.

### INFO-1: `s_deleted` is updated without synchronization; the only effect is false negatives

- **File:line:** `fake_support.c:252` (a plain volatile array), `255-262`
  (`sem_note_live`) and `332-344` (`vSemaphoreDelete`).
- **Analysis:**
  - **Concurrent deletes.** Two deletes on different threads can both pick the
    same free slot, and one record is lost. A lost record can only hide a bad
    call (a false negative).
  - **No false positives.**
    - `vSemaphoreDelete` records the handle before `CloseHandle`. Windows can
      therefore only reuse the value after it is recorded.
    - `sem_note_live` runs after `CreateSemaphore` and clears any matching
      slot. So delete-then-recreate at the same handle value is handled
      correctly, which matches FreeRTOS, where a recreated semaphore is a
      fresh valid object.
    - A recorded value later reused by a non-semaphore object (a thread or
      event handle) stays marked. This is harmless, because the fake never
      passes such a handle to the semaphore functions.
  - **Silent drop when the table is full.** Once all 64 slots are used, a new
    delete is dropped silently. The instrumented peak was 12, so this is
    theoretical.
  - **Not FreeRTOS semantics.** A real `xSemaphoreTake` on a deleted handle is
    undefined behavior (use after free), not a pdFALSE. The fake's
    count-and-fail is the right test oracle, but it does not model the
    target.
- **Fix (optional):** Claim slots with `InterlockedCompareExchangePointer`, and
  print a FAIL line when the table overflows instead of dropping the record.

### INFO-2: ExitProcess after 5000 bad calls cannot hang the runner or hide a failure

- **File:line:** `fake_support.c:272-277`.
- **Analysis:**
  - The FAIL line printed before `ExitProcess(1)` matches the runner's and
    negtest's FAIL patterns, and the exit code is non-zero.
  - Each bad call sleeps 1 ms, so a spinning writer reaches 5000 hits after
    at least 5 s. By then `main` has normally already returned with the test's
    own FAIL at 516.
  - `stdout` is flushed before the exit, so the message is never lost.
- **Note:** `g_fr_sem_bad_n` is cumulative over the whole executable, so a bad
  call in an earlier test would be reported at 516. That is acceptable, but
  snapshotting the count at the start of `test_writer_wedge` would make the
  attribution exact.

### INFO-3: the `bytes_total` check hard-codes 20000

- **File:line:** `test_update_fetch.c:467-468`.
- **Analysis:** The check is `strstr(resp, "\"bytes_total\":20000") &&
  IMG_LEN == 20000u`. If IMG_LEN changes, the check fails loudly rather than
  passing vacuously, so it is safe. Building the expected string from
  `IMG_LEN` would remove the extra edit. The check proves that WR_BEGIN
  returned OK before the wedge, which stands in for `stage_begun`. M7 shows
  the check is sensitive: it fails at 468 when `stage_begun` is never set.

### Include drift: none

`update_http_internal.h` is already included by `update_fetch.c` and
`update_http.c`. It declares `update_fetch_writer_wedged()` with the same
signature the removed forward declaration had. The `test_update_http_refusals`
build, which `#include`s `test_update_fetch.c`, still compiles and passes. A
future signature change now breaks the test build instead of silently linking
a mismatched declaration.

## Disposition

The commit can stay. LOW-1 is pre-existing and worth a small follow-up: a
longer bound on the positive writer-start waits. LOW-2 and the INFO items are
optional hardening.

## Fix status

| Finding | Status |
|---|---|
| LOW-1 | FIXED in `aba482d76` (30 s `WAIT_FOR_MS` at the two positive wedge waits; the iteration-counted fake fallback remains, see `REVIEW_FLAKEFX4_2026-10-12.md` INFO-2) |
| LOW-2 | FIXED in `aba482d76` (idle-waiter handshake replaces `Sleep(50)`; negative-tested in `REVIEW_FLAKEFX4_2026-10-12.md`, A5 CAUGHT / A6 MISSED) |
| INFO-1..3 | no action required |
