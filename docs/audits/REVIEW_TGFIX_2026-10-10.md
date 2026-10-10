# Review: test-gap fixes C1/C2/C3/B2/B6/A4 (3b2e8b7327)

Date: 2026-10-10. Reviewer: Claude Opus 5.5. Reviewed at origin/dev 3f952a53f.

Commit 3b2e8b7327 closes the test-gap LOWs C1, C2, C3, B2, B6 and A4 from
`docs/audits/REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10.md`.

**Verdict:** no HIGH or MED findings. Every new test or guard is real: each one was
negative-tested with `tools/negtest.ps1` (results below), and each mutation was caught.
There is one LOW (C1 has a silent-green failure mode if registration is lost) and some NITs.

## Negative tests run for this review

Every run used `tools/negtest.ps1` in throwaway copies. Each baseline passed, and each run reported `real_tree_unchanged`.

| Item | Mutation | Command | Result |
|---|---|---|---|
| C1 | `Complete-HostBuilds` skips running `thermo_task_tests.exe`, so it has no exit code | SaftyFW `build_host_tests.ps1`, judged by exit code | CAUGHT: exit 1, `FAILED -- thermo_task_tests.exe (exit 1)` |
| C1 | `hal_spi_pico_tests.exe` (a middle executable) forced to exit 3 | same | CAUGHT: exit 3 |
| C1 sanity | the same skip, plus `else { 1 }` changed to `else { 0 }` | same | MISSED (expected): exit 0, `all passed`. This shows the first CAUGHT comes from the new missing-entry branch. |
| C3 | `OVUV` dropped from the `s5_bad_read_now()` mask (`safety_guards.c:131`) | same | CAUGHT: `test_safety_guards.c:239: OVUV` |
| C2 | `th_reset(); th_run_captured_task();` injected at the start of `run_test_watchdog_task_loop` | same | CAUGHT: exit 2, `task_harness: th_run_captured_task() with no captured task` |
| C2 | `th_reset(); th_abort();` injected at the same place | same | CAUGHT: exit 2, `task_harness: th_abort() outside a th_run_captured_task() run` |
| B2 | autotune branch `if at.state not in (0, 5, 6)` changed to `if False` (`mcp_server_debug.py`) | pytest on `test_pctools_batch_d_2026_10_10.py` | CAUGHT: `test_debug_program_pico_refused_during_autotune` |
| B6 | the BASELINE-FAILED listing loop removed from `Invoke-Chunk` (`negtest.ps1:489-493`) | `check_negtest.ps1 -Group C` | CAUGHT: `baselinefail_par: expected both mutations listed ..., got 1` |
| A4 | `roundf(convert(c))` changed to `convert(roundf(c))` | KilnFW host tests, `-Only test_ui_page_home_rail` | CAUGHT: lines 105 and 109 (F display and rate rounding) |
| A4 | lower clamp removed | same | CAUGHT: line 100 (`unit_entry_f_rate_lower_clamp`) |
| A4 | upper clamp removed | same | CAUGHT: line 102 (`unit_entry_f_rate_upper_clamp`) |

C2 verdict: the guard is real. Both misuse paths now end the executable with exit 2,
and the build script counts that as a failure. Before this change, a run with no task
returned silently, and `th_abort()` outside a run longjmp'd through an uninitialised
`jmp_buf`. No existing test reaches either path, because the baseline passes and a hit
would exit 2. So the guard protects future tests and changes no current behaviour.

## Rebase resolution

- `firmware/SaftyFW/test/build_host_tests.ps1`: the hand-written list that was removed
  had 10 executables, including `discrete_task_tests.exe` from the concurrent ba267a692.
  All 10 are still registered through `Add-HostBuild`. The kilnlink fuzz seed hint is kept.
  The resolution is correct.
- `tools/PcTools/tests/test_pctools_batch_d_2026_10_10.py`: the test count went from 27
  to 30. There are no duplicate `def test_` names, so no test silently shadows another.
  The B1 tests from 7cc343260 are intact (`..._unreadable_esp_proceeds_with_warning`,
  `..._confirmed_running_refused_unreadable_does_not_mask`,
  `..._confirmed_running_allow_running_overrides`). The three new B2 tests are added
  next to them.

## Findings

### LOW-1: C1 derived list goes silently green if registrations are lost (FIXED in dfd3a346f)

`firmware/SaftyFW/test/build_host_tests.ps1:300,663-680`. `$results` is built only from
`$allHostNames`. If that list ends up empty, `$results` is empty, the script prints
`SAFTYFW HOST TESTS: all passed` and exits 0, whatever the executables returned.

Example: `$script:allHostNames += $Name` (line 300) is refactored to
`$allHostNames += $Name`. That appends to a function-local copy, and the script-scope
list stays `@()`. I found this by reading the code; it was not negtested. The old
hand-written chain could not fail this way.

Suggested fix: fail when `$allHostNames.Count` is 0 or differs from the number of queued
builds, or iterate `$exitCodes` as well and fail on any name present in only one of the two.

### NIT (all FIXED in dfd3a346f: dead $mainExit removed, "did not run" marker, duplicate rail check removed, with-statement reformatted, S5 failure message names the bit, nested-run guard, direct S5 try_clear test)

- `build_host_tests.ps1:651`: `$mainExit` is now dead (assigned and never read).
- `build_host_tests.ps1:664`: an executable that never ran is reported as `(exit 1)`, the
  same as a real test failure. A distinct marker such as `(no exit code: never ran)` would
  make the log clearer.
- `test_ui_page_home_rail.c:97-98`: the new `unit_entry_f_lower_clamp` check repeats the
  existing check at lines 90-91 exactly (same call, same arguments, -500 F to 20 C). The
  A4 finding called the existing clamp checks Celsius-only, but that one was already
  Fahrenheit. The new duplicate adds nothing; the rate clamp and F rounding checks do.
- `test_pctools_batch_d_2026_10_10.py:84`: the `with` statement in `_pico_program` lost its
  backslash continuations. It is now one 456-character line with space runs. It is valid
  Python but hard to read and diff.
- `test_safety_guards.c:242`: the reset check uses the same message on every iteration, so
  a failure does not name the bit. Line 239 does name it.
- `task_harness.c:55-58`: a nested `th_run_captured_task()` would clear `s_running` and
  overwrite `s_jmp` for the outer run. No test nests runs, so this is noted only.
- C3 covers the `safety_guards_tick()` path. The same `s5_bad_read_now()` helper also
  gates `safety_guards_try_clear()`. That path is covered implicitly because it is the
  same function, not by a direct test.

## Not vacuous

Each new assertion fails when the code under test is removed or weakened (table above).
The B2 idle and override tests are positive tests: they pass with the guard removed by
design, and the refusal test is the one that pins the guard. The B6 assertion fails on the
pre-fix `negtest.ps1`.
