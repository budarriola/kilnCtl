# Review: flakefx commits (REVIEW_FIRELOWFX3 LOW-3 / LOW-4), 2026-10-11

Scope: `1a057a7be`, `023f99f03`, `71fa51827` on origin/dev. These commits fix two
load-sensitive KilnFW host-test flakes:

- `test_web_auth_store.c`: `time_verify_calls()` now times with QueryPerformanceCounter
  and takes the minimum of 9 batches per path (the first commit used `GetThreadTimes`; the
  second replaced it). The three assertions are unchanged.
- `stubs_update_fetch/fake_support.c`: during the wedge test, the fake `xSemaphoreTake`
  starts the scaled writer-timeout clock only once a writer thread is parked in fake flash.

Reviewed in a clean worktree at `71fa51827`. Nothing touched the board.

## Verdict

No HIGH or MEDIUM findings. The fixes are test-only, the tests still catch the defects
they were written for, and both executables passed 60 of 60 runs under load. Three LOW
and three INFO notes follow. None of them is a regression introduced by these commits.

## Questions answered

1. **Do the tests still catch the real defects?** Yes, both of them. The negtest results
   are below.
2. **Did production code change?** No. `git show --stat` on all three commits lists only
   `test/test_web_auth_store.c`, `test/stubs_update_fetch/fake_support.c` and the
   FIRELOWFX3 audit doc. `update_fetch.c` is untouched. Its real wait is still
   `xSemaphoreTake(wr_done, pdMS_TO_TICKS(FETCH_WR_TIMEOUT_MS = 30000))` from the moment
   the op is issued, followed by the 1000 ms Review-5 L1 grace take. A wedged writer is
   detected no later than before.
3. **Is the fake faithful?** It differs from FreeRTOS on purpose, and only in a narrow
   case. The gated path runs only when all of these hold: `g_fr_time_div > 1`,
   `ticks >= 30000`, `ticks != portMAX_DELAY` and `g_fr_flash_write_block`. Only
   `test_writer_wedge()` sets `time_div` and `flash_write_block`. It runs last, and
   `fr_reset()` sets `time_div` back to 1. Every other wait in the linked sources is either
   `portMAX_DELAY` or the 1000-tick L1 grace take, and neither reaches the gated path.
   Inside the gate, the 1 ms polling `WaitForSingleObject` still returns `pdTRUE` at once
   when the semaphore is given. Two executables link `fake_support.c`:
   `kilnctl_host_tests_update_fetch.exe` and `kilnctl_host_tests_update_http_refusals.exe`.
   The second one `#include`s `test_update_fetch.c` and runs its `extra_tests()` before the
   wedge case, and none of those touch `g_fr_time_div` or `g_fr_flash_write_block`. No
   other test's timeout path is hidden.
4. **Are the tests robust under load?** Yes. With the CPU at 71-72% load from other
   sessions, each of the three affected executables passed 20 runs (5 at a time) and then
   40 runs (all 40 at once). Total: 0 of 180 runs failed. The largest wall time was 3.7 s
   (web_auth_store, 40 at once).

## Negtest results

Command: `tools\negtest.ps1 -Command "<build_host_tests.ps1 -OutDir {OUT} -Only '^(web_auth_store|update_fetch|update_http_refusals) '>" -Mutations <json> -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
This is the `kilnfw-host` preset command with `-Only` added, so that the full suite's
unrelated load-sensitive executables could not fail the baseline on this machine. Each
mutation run used a fresh `{OUT}`. Every baseline passed, the real tree was unchanged
afterwards and every negtest copy was removed.

| Mutation (production code) | HEAD `71fa518` | pre-fix `bab554b` |
|---|---|---|
| W1 the original defect: `if (!loaded_ok \|\| !rec.configured) return false;` before PBKDF2 | CAUGHT (test:412) | - |
| W2 decoy iterations `WEB_AUTH_ITERATIONS` -> `1u` | CAUGHT (test:412) | - |
| W3 decoy iterations `/10` (a 10x asymmetry, to probe sensitivity) | CAUGHT (test:412) | - |
| U1 `wedged = false`, arbiter call kept | MISSED (see INFO-1) | MISSED |
| U1b `wedged = false`, arbiter call dropped (the form `71fa518` cites) | CAUGHT (test:504) | CAUGHT |
| U2 writer wait `portMAX_DELAY` (a wedge is never detected) | CAUGHT (test:463 ff.) | - |
| U3 timeout path does not latch `wr_wedged` | CAUGHT (test:463 ff.) | - |
| U4 `wr_call()` ignores `wr_wedged` (overwrites the abandoned command block) | MISSED (LOW-1) | MISSED |

U1, U1b and U4 give the same result before and after the fix, so the fake's change did
not weaken anything.

## Findings

### LOW-1: the `wr_call()` wedge guard is not pinned by any test (pre-existing)

Removing `if (s_c->wr_wedged) return UPDATE_STAGE_ERR_FLASH;` from `wr_call()`
(`update_fetch.c:439`) passes both executables, at HEAD and before the fix. The comment
on the guard says that without it, the job's final `WR_ABORT` overwrites `wr.cmd`, the
late op skips its undo, and the stage stays UPLOADING. In the fake this does not happen.
The overwriting call also gives `wr_req`, so after the parked op finishes the writer runs
the queued `WR_ABORT`, which aborts the owned upload and brings the stage back to IDLE.
Line 504 is therefore satisfied by a different route. Either the comment describes an
older design, or the failure needs an interleaving the test does not produce. To fix,
add an assertion that tells the two routes apart, for example that no second writer op
was issued after the wedge (`update_wr_arb` issue count, or a fake count of `wr_req`
gives). Another option is to drop the guard's claim from the comment if the queued abort
is the intended recovery. This is not a regression from these commits.

**Fix status:** fixed in `72faacb41`. The fake `xSemaphoreTake` counts writer-op waits begun after the wedge (`g_fr_wr_wait_after_wedge`) and `test_writer_wedge()` asserts 0. Re-negtested in REVIEW_FLAKEFX2_2026-10-12 M1: CAUGHT at test:501 in both executables. Related gaps that remain are REVIEW_FLAKEFX2 LOW-1..3.

### LOW-2: the fix status in `71fa518` overstates the negtest

It says "forcing `wedged = false` in `update_fetch.c` is CAUGHT". That holds only when
the `update_wr_arb_caller_timeout()` call is dropped too (U1b). U1b is caught at
test:504 because the arbiter never learns of the timeout, so the late op is not treated
as abandoned and does not undo itself. The wedge-detection branch is not what catches it.
With the arbiter call kept (U1), the mutation is MISSED. See INFO-1 for why that is
acceptable. The record should name the exact mutation that was run.

**Fix status:** fixed in `72faacb41`. The FIRELOWFX3 line now names U1b and U1. Its `test:504` citation is now test:508 (REVIEW_FLAKEFX2 INFO-1).

### LOW-3: the fake's gate is tied to the literal 30000

`ticks >= 30000` stands in for "this is the FETCH_WR_TIMEOUT_MS wait". If
`FETCH_WR_TIMEOUT_MS` is lowered below 30 s, the fake quietly returns to timing from
the call, and LOW-4's original race returns as a flake. The test would not become
vacuous. A shared constant, or a fake hook keyed on the `wr_done` handle, would remove
the coupling.

**Fix status:** fixed in `72faacb41`. `FETCH_WR_TIMEOUT_MS` moved to `update_fetch.h`, and the fake gate tests `ticks >= pdMS_TO_TICKS(FETCH_WR_TIMEOUT_MS)`. REVIEW_FLAKEFX2 M5 (20000u) still passes.

### INFO-1: U1 is an almost-equivalent mutant

With `wedged = false` and the arbiter call kept, the caller falls into the Review-5 L1
grace take. That take is 1000 ticks, 1 ms in the scaled fake, and it latches `wr_wedged`
the same way with a different log line. In production this would only delay detection by
1 s. No test change is needed.

### INFO-2: the fallback timing in the fake's comment depends on the timer

The fake counts 1 ms `WaitForSingleObject` slices. Under Windows' default 15.6 ms timer
resolution (per process since Windows 10 2004), one slice can take about 15 ms. The
scaled budget can then stretch from 30 ms to roughly 0.5 s, and the "15 s" fallback (for a
writer that never parks) to as much as about 4 min. The fallback only matters for a
regression in which the writer never reaches flash. The test's 5 s `WAIT_FOR` would fail
first and report that, so a regression is still caught, just with a slower background
thread. Measuring elapsed time with `GetTickCount64` instead of counting slices would make
the comment accurate.

### INFO-3: the first timing assertion was already weak, and still is

`unconfigured_s > no_op_s * 3.0` compares against a minimum no-op batch of 20 NULL-password
returns. That is about 0-1 QPC ticks, and under `clock()` it was 0. This bound alone would
pass almost any non-zero cost. The real defect (W1) is caught by the same-order-of-magnitude
assertion at test:412, which is meaningful and now stable. No change is needed. A floor
such as "unconfigured_s at least half of wrong_password_s" would make the intent explicit.

## Evidence

- Worktree `C:\wt\rvflk_zztsmb` at `71fa51827`. Host tests were built through
  `build_host_tests.ps1 -Only` and the build gate. All 3 executables passed: web_auth_store
  174/174, update_fetch 231/231, update_http_refusals 398/398 (sometimes 399/399, because of
  a load-dependent `r == 202` branch in the wedge test).
- Negtest logs: `%TEMP%\negtest_logs\20261011_235408_6wbr` (W1-W3, U1-U4 at HEAD), plus
  the two U1/U1b/U4 runs at HEAD and at `bab554b450` (`1a057a7be~1`).
