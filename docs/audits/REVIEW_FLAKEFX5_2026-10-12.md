# Review: flakefx5 (update_fetch host-test scaffolding), 2026-10-12

Commit under review: `8eb117f9c` on origin/dev ("update_fetch host tests: drop stale writer-exit comment, reset
idle-waiter counter in fr_reset, wall-clock fallback in scaled writer wait"). Test scaffolding only:
`firmware/KilnFW/App/test/test_update_fetch.c`, `firmware/KilnFW/App/test/stubs_update_fetch/fake_support.c`.

**Severity counts: HIGH 0, MED 0, LOW 0, INFO 3.**

## Questions checked

1. **Does zeroing `g_fr_sem_idle_waiters` in `fr_reset()` desync a live waiter?** Only if a thread is parked in an
   unbounded take when `fr_reset()` runs. It would then decrement to -1 on wake, and the wedge test's
   `>= 1` handshake would time out after 30 s. That is a loud false FAIL, never a false pass. No such thread exists
   today. The writer task is per-job: `wr_stop()` sends `WR_EXIT` and waits on `wr_done`, which the writer gives
   only after its take returned and decremented. The only other unbounded take is the `st_lock()` mutex, and the
   main thread calling `fr_reset()` cannot be inside it. The wedged writer is the one task that outlives a job,
   and `test_writer_wedge` runs last. Before this change the counter was balanced too, so the reset is a no-op in
   the current suite (the `drop_idle_reset` mutant below confirms it). INFO-1.
2. **Does the 15 s wall-clock fallback bring back flakiness, or let a hang pass silently?** No. The fallback fires
   only if no writer has parked in flash after 15 s. Even then the wedge is declared and
   `CHECK(g_fr_flash_blocked_n >= 1, "the abandoned writer is still parked in flash")` still has to pass, so a
   writer that never parks fails loudly. At worst the release takes 15 s plus a 30-slice budget, which is inside
   the 30 s `WAIT_FOR_MS` ceiling. The old fallback counted 15000 iterations of `WaitForSingleObject(...,1)`.
   With Windows' default 15.6 ms timer, that could take about 234 s, far past the ceiling, so the new form is
   strictly better under load. The budget counter (`started`) still counts iterations, so under starvation the
   wait only gets longer, never shorter. A slower wait cannot declare a wedge early. INFO-2.
3. **Is `GetTickCount64` available in every toolchain?** Yes. Both executables that compile `fake_support.c`
   (`update_fetch` and `update_http_refusals`) are built with MSVC `cl` (`build_host_tests.ps1`). The stub
   already depends on `<windows.h>`/`_beginthreadex`, and the function has existed since Vista, which is below the
   SDK default target. No MinGW/gcc path builds these stubs.

## INFO

- **INFO-1** `fr_reset()` assumes no task is parked in an unbounded take. A cheap guard would be
  `CHECK(fr_wait_tasks_idle(...))` in `fixture()` before the reset. Not required today.
- **INFO-2** The `fallback_immediate` mutant (clock starts at once) passes. On an unloaded machine the writer
  parks well inside the 30 ms scaled budget. The parked-writer gate only adds robustness under starvation, so no
  test can observe it on an idle machine. This is expected.
- **INFO-3** Removing the stale comment at `test_update_fetch.c:518` is correct. The comment that follows
  describes the idle-wait handshake.

## Runs

- `build_host_tests.ps1 -Only update_fetch` three times in a row (3 executables each: update_fetch,
  update_fetch_heap, update_http_refusals): **3/3 PASS**, `test_update_fetch: 404 checks, 0 failed` every time.
  A negtest full host-test run was running at the same time, which added load.
- `tools\negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`:
  baseline PASS. `fallback_immediate` (`>= 15000` -> `>= 0`): **MISSED**, unobservable by design (INFO-2).
  `drop_idle_reset` (reset line -> `(void)0;`): **MISSED**, unobservable because the counter is already 0 at every
  reset (INFO-1). Both mutants compiled and ran (`0 failed`). real_tree_unchanged=true, copies_removed=true.
