# Review: REVIEW_FLAKEFX LOW-1..3 fix (`72faacb41`), 2026-10-12

Scope: `72faacb41` on origin/dev ("update_fetch tests: pin wr_call wedge guard, tie fake
gate to FETCH_WR_TIMEOUT_MS, correct fix-status note"). It changes:

- `stubs_update_fetch/fake_support.c/.h`: a `g_fr_wr_wait_after_wedge` counter in the fake
  `xSemaphoreTake`, incremented when a writer-op wait starts after the wedge latched.
- `test_update_fetch.c` `test_writer_wedge()`: `CHECK(g_fr_wr_wait_after_wedge == 0)`.
- `FETCH_WR_TIMEOUT_MS` moved from `update_fetch.c` to `update_fetch.h`. The fake gate now
  tests `ticks >= pdMS_TO_TICKS(FETCH_WR_TIMEOUT_MS)` instead of the literal 30000.
- A corrected fix-status line in `REVIEW_FIRELOWFX3_2026-10-11.md`.

Reviewed adversarially in a minted worktree (`C:\wt\flakefx2_0usvc2`, origin/dev
`5033604f3`, which contains `72faacb41`). Nothing touched the board.

## Verdict

No HIGH or MEDIUM findings. The new counter is not vacuous: removing the `wr_call()` guard
is now CAUGHT at test:501 in both executables that run the wedge case, and nothing else
catches it (REVIEW_FLAKEFX U4). The header move changes no production behavior. The test
did not flake in 210 runs under load. The three LOW findings below are gaps in the same
wedge test that this commit did not introduce. Each one lets a related regression pass.

## Questions answered

1. **Does the counter model "a wait after the wedge"? Can it pass vacuously?** It counts
   any wait of at least `FETCH_WR_TIMEOUT_MS` that starts while `wr_wedged` is set and the
   fake flash is blocked. Only `wr_call()` makes such a wait (`update_fetch.c:455`). The
   L1 grace take is 1000 ticks, and every other take in `drivers/update/*.c` is
   `portMAX_DELAY`, so a guarded build cannot produce a false count. The wedging op's own
   wait started before `wr_wedged` latched, so it is not counted either. The counter is
   reached through one path only: `fetch_task()` issues `WR_ABORT` after the wedge because
   `stage_begun && !stage_done` holds. `wr_stop()` has its own wedge check, so `WR_EXIT`
   never reaches `wr_call()`. Today the parked op is the first flash write, the head
   flush inside `WR_WRITE`. `upload_begin` only erases, and erases do not block in the
   fake. So `stage_begun` is true and the path is taken. M1 proves this. The test does
   not assert that precondition, though (LOW-3).
2. **Is it flaky under load?** No failures. At 86-91% CPU load (this review's own target
   builds plus other sessions), 4 batches of 30 parallel runs passed: `update_fetch` and
   `update_http_refusals`, 2 rounds each. Then 3 batches of 30 `update_fetch` runs passed
   at Idle priority class, to starve the writer thread. That is 210 runs with 0 failures,
   and every batch finished in 1-2 s wall time. The new check cannot add a parking race.
   It reads a counter that is final before the job reports `failed`, and the test waits
   for `failed` and for the claim release before the check runs. The writer cannot finish
   while it is parked on `WaitForSingleObject(INFINITE)`, so the 30 ms scaled budget is
   deterministic once the clock starts. The check count varies (232 or 233 for
   update_fetch, 398 or 399 for refusals). That comes from the existing load-dependent
   `r == 202` branch REVIEW_FLAKEFX noted, not from this commit.
3. **Does the header move change any production include or ODR?** No. It is a macro, so
   it has no linkage and no ODR. Only `update_fetch.c` and `update_http.c` include
   `update_fetch.h` in production, plus the test fake `fake_support.c`. No other file
   defines the name, so no redefinition is possible. `update_chain_lint.py` checks only the use site
   (`pdMS_TO_TICKS(FETCH_WR_TIMEOUT_MS)` in `update_fetch.c`), and that still matches. The
   recovery image does not compile `update_fetch.c` or include `update_fetch.h`: its
   `main/CMakeLists.txt` takes only `update/stage_header.c` and `update/update_semver.c`
   from `drivers/update`. Both target builds pass (below).
4. **Is the gate now tied to the constant?** Yes. M5 sets `FETCH_WR_TIMEOUT_MS` to
   `20000u`, and the full host suite still passes. That is the expected equivalence, and
   it shows the fake follows the constant.

## Findings

### LOW-1: unchecked `WAIT_FOR`s let a second writer start after the wedge (M4 MISSED)

In `test_writer_wedge()`, `WAIT_FOR(g_fr_live_tasks <= 1)` at test:482 and test:486 is a
bounded wait with no `CHECK` after it. If `wr_start()`'s wedge refusal is dropped (M4),
the second download starts a fresh writer task. That task overwrites
`s_c->wr_req`/`wr_done`, which the parked writer still uses. Every op is then refused by
`wr_call()`'s guard, and `wr_stop()` returns early, so the new writer task and its two
semaphores leak on every job until reboot. The job still ends with
`writer_wedged_reboot_required` and its buffers are freed, so every assertion passes.

Fix: follow each of those two waits with
`CHECK(g_fr_live_tasks <= 1, "only the parked writer remains")`. A leaked writer is a
live task blocked on `wr_req` forever, so `g_fr_live_tasks` stays at 2.

### LOW-2: nothing pins the parked writer's semaphores against `wr_stop()` (M3 MISSED)

If `wr_stop()`'s `|| s_c->wr_wedged` is dropped (M3), the job's teardown deletes
`wr_req`/`wr_done` and sets them to NULL while the abandoned writer is parked. Once the
writer is released, it gives a NULL/deleted `wr_done` and loops on a NULL `wr_req`. In
the fake, `WaitForSingleObject(NULL)` fails at once, so the writer re-runs its last
command in a busy loop. On FreeRTOS this is a use-after-free or an assert. The test
passes anyway: the stage still returns to IDLE, and the spinning thread dies with the
process. The M3 run took 1099 s against a 688 s baseline, which is consistent with a
spinning thread competing for CPU.

Fix: in `fake_support.c`, record handles closed by `vSemaphoreDelete` and count any
`xSemaphoreTake`/`xSemaphoreGive` on a NULL or closed handle (`g_fr_sem_bad_n`). Then add
`CHECK(g_fr_sem_bad_n == 0)` at the end of `test_writer_wedge()`, after the release and
`WAIT_FOR(stage_phase() == UPDATE_STAGE_IDLE)`. Optionally also check that
`g_fr_flash_write_n` stays stable across a short sleep after the release.

### LOW-3: the new check's reach depends on an unasserted precondition

`g_fr_wr_wait_after_wedge` can only move if `fetch_task()` issues the post-wedge
`WR_ABORT`. That requires the wedge to land on an op after a successful `WR_BEGIN`
(`stage_begun`), and today it does only because `update_stage_upload_begin()` writes no
flash. If begin ever writes, for example by flushing a header early, the wedge moves to
`WR_BEGIN`. Then `stage_begun` stays false, no `WR_ABORT` is issued, and test:501 passes
even with the guard removed. Nothing in the test would notice.

Fix: after `WAIT_FOR((job_result(), strcmp(g_state, "failed") == 0))`, assert that the
failed job had passed `WR_BEGIN`. `st_set_progress(0, app_size)` runs only after
`stage_begun = true`, and `st.total` is cleared only at the next job start (the
`memset(&s_c->st, ...)` at `update_fetch.c:925`). So
`CHECK(strstr(g_rq.resp, "\"bytes_total\":" STR(IMG_LEN)) != NULL, "wedge hit after WR_BEGIN, so the post-wedge WR_ABORT path is exercised")`
is enough, with the literal formatted from `IMG_LEN`.

### INFO-1: the new FIRELOWFX3 fix-status line cites a line number this commit moved

It says U1b is "CAUGHT (test:504)". That was the line at `71fa518`. `72faacb41` inserts
four lines above it, and the assertion ("abandoned op aborted its own upload") is now
test:508, which is where this review's M2 caught. Cite the assertion text, not the line.

### INFO-2: cosmetic

- `fr_flash_release()` in `fake_support.c` gained a stray 8-space indent on
  `if (s_flash_ev != NULL) {`. Re-indent it to 4.
- `fake_support.c` now includes `update_fetch.h`, which already declares
  `update_fetch_writer_wedged()`. The added forward declaration is redundant. Drop it.
- A test-motivated macro now lives in the public `update_fetch.h`. That is harmless, and
  the alternative is an internal header. Leave it.

## Evidence

- Worktree `C:\wt\flakefx2_0usvc2`, HEAD `5033604f3` (origin/dev, contains `72faacb41`).
- Full KilnFW host suite: `build_host_tests.ps1 -OutDir ...` reported "Built: 98/98
  executables / all 98 host test executables built and passed".
- `check_00_kilnfw_target_build.ps1`: PASS, `KilnCtrl.bin` 2949328 B, 30% of `app` free.
- `check_00_kilnfw_recovery_target_build.ps1`: PASS, `recovery.bin` 783824 B.
- Stress: 210 parallel runs (see question 2), 0 failures.
- Negtest: `tools\negtest.ps1 -Preset kilnfw-host -Mutations <appendix> -ExpectPattern
  "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. The baseline passed
  (688 s), the real tree was unchanged afterwards, the copies were removed, and the logs
  are in `%TEMP%\negtest_logs\20261012_004304_tyy8`.

| Mutation | Result | Caught at |
|---|---|---|
| M1 `wr_call()` wedge guard disabled (`if (0 && s_c->wr_wedged)`) | CAUGHT | test:501, the new counter (update_fetch and update_http_refusals) |
| M2 `wr.cmd` written before the guard (the defect the guard's comment describes) | CAUGHT | test:508, abandoned op's undo |
| M3 `wr_stop()` ignores the wedge | MISSED | LOW-2 |
| M4 `wr_start()` ignores the wedge | MISSED | LOW-1 |
| M5 `FETCH_WR_TIMEOUT_MS` 30000 -> 20000 | MISSED (expected: equivalence, the fake follows the constant) | - |

## Appendix: mutation JSON

```json
[
  {"name":"M1_wr_call_guard_removed","file":"firmware/KilnFW/App/drivers/update/update_fetch.c",
   "find":"    if (s_c->wr_wedged) {\n        // Before touching the command block",
   "replace":"    if (0 && s_c->wr_wedged) {\n        // Before touching the command block"},
  {"name":"M2_cmd_written_before_guard","file":"firmware/KilnFW/App/drivers/update/update_fetch.c",
   "find":"    if (s_c->wr_wedged) {\n        // Before touching the command block",
   "replace":"    s_c->wr.cmd = cmd;\n    if (s_c->wr_wedged) {\n        // Before touching the command block"},
  {"name":"M3_wr_stop_ignores_wedge","file":"firmware/KilnFW/App/drivers/update/update_fetch.c",
   "find":"    if (s_c->wr_req == NULL || s_c->wr_wedged) {",
   "replace":"    if (s_c->wr_req == NULL) {"},
  {"name":"M4_wr_start_ignores_wedge","file":"firmware/KilnFW/App/drivers/update/update_fetch.c",
   "find":"        return false; // a previous writer op never returned; reboot to recover",
   "replace":"        (void)0; // M4: wedge ignored"},
  {"name":"M5_timeout_20s_equivalence","file":"firmware/KilnFW/App/drivers/update/update_fetch.h",
   "find":"#define FETCH_WR_TIMEOUT_MS 30000u",
   "replace":"#define FETCH_WR_TIMEOUT_MS 20000u"}
]
```

## Fix status

| Finding | Status |
|---|---|
| LOW-1 | fixed: `CHECK(g_fr_live_tasks <= 1)` after both waits in `test_writer_wedge()` |
| LOW-2 | fixed: fake counts take/give on NULL or deleted semaphores (`g_fr_sem_bad_n`), sleeps 1 ms and exits after 5000 hits instead of spinning; test checks it is 0 after the release |
| LOW-3 | fixed: failed job status must show `"bytes_total":20000` (IMG_LEN) |
| INFO-1 | fixed: FIRELOWFX3 line cites the assertion text |
| INFO-2 | fixed: indent corrected; the forward declaration is replaced by `#include "update_http_internal.h"` (it is not in `update_fetch.h`, so the review's "redundant" claim was wrong) |

Negtest M1..M4: see the landing commit message.
