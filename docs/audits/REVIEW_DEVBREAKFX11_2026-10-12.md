# Review: DEVBREAKFX11 (`40fb6ea87` + `de56f4401`), 2026-10-12

Adversarial review of the REVIEW_DEVBREAKFX10 LOW-1/LOW-2/LOW-3/INFO-2 fixes
on origin/dev:

- `40fb6ea87`: `tools/host_test_verdict.ps1` compares `Built + BuildFailures`
  to `Expected` and no longer suppresses MISMATCH under `-Only`;
  `tools/check_host_test_verdict.ps1` gains new verdict cases and static
  call-site assertions on `firmware/KilnFW/App/test/build_host_tests.ps1`;
  `tools/check_build_gate_reentrant.ps1` writes the ready file to `.tmp`,
  renames it into place, and reads it null-safe.
- `de56f4401`: audit doc status only.

Reviewed at dev tip `2d9cdae82` in a worktree minted from origin/dev. Line
numbers are at that tip.

**Verdict: no HIGH, no MED. 2 LOW, 4 INFO.** The fixes are correct for the
cases they name. One new false MISMATCH comes from the LOW-2 fix itself
(LOW-1). The static call-site assertions catch edits to the three verdict
lines but not a bypass placed anywhere else (LOW-2).

## Evidence

- **Full run (Q5).** `build_host_tests.ps1` with no `-Only`, `-OutDir` in
  scratch: `Built: 99/99 executables`, `all 99 host test executables built
  and passed`, no MISMATCH, exit 0.
- **`-Only` typo (Q3).** `-Only main_reset_fenceXX` printed `-Only
  'main_reset_fenceXX' matched no host-test executable` and exited 1.
- **Compile error in the selected test (Q2).** In a negtest copy,
  `test_main_reset_fence.c:87` was changed to `TEST_CHECK(s_phase_seen == 4 +,`
  and run with `-Only main_reset_fence`. The log shows the cl error C2059,
  `Built: 0/1`, `BUILD FAILURES (1)`, no MISMATCH, and exit 1. Built does
  exclude build failures: `Invoke-HostTestRun` (line 547) is the only place
  that appends to `$script:builtExes`, and both build-failure paths (lines 584
  and 621) `return`/`continue` before reaching it.
- **`-Only` with a smaller selection (Q3).** Under `-Only`, `$totalExpected`
  is `$script:onlySelected.Count` (line 3591). That count is taken from the
  filter, not from all 99 executables, so a legitimate small selection gives no
  MISMATCH. The baseline `-Only main_reset_fence` run passed with exit 0.
- **LOW-3 (Q4).** On PS 5.1 in scratch, `Move-Item -Force` onto an existing
  file overwrote it (`after force: got=True`). Without `-Force` it failed with
  "Cannot create a file when that file already exists". The child passes
  `-Force`. The stale-file removal is still at line 126. It runs before the
  holder child starts, so the rename target never exists in practice.

## Findings

### LOW-1: `sim_credibility_gate_closedloop` build failure gives a false MISMATCH, and can hide a real dropout (FIXED, devbreakfx12)

`build_host_tests.ps1:3077` appends `sim_credibility_gate_closedloop` to
`$script:buildFailures`, but that item is not one of the 99 counted in
`$totalExpected`. It is a compile-only signal built outside
`Invoke-HostTestExe`, and it is built even under `-Only`. Since the LOW-2 fix
adds `BuildFailures` to `Built`, this one item makes the total one too high:

- A closedloop compile break alone now prints `MISMATCH: 1 built + 1 build
  failures = 2 but 1 were expected -- at least one was silently never
  attempted`. Negtest R5 (below) shows this under `-Only main_reset_fence`. A
  full run would print `99 + 1 = 100 but 99`. The exit code is correctly 1,
  but the text claims a dropout that did not happen.
- A closedloop break and one real dropout at the same time cancel out
  (`98 + 1 = 99`), so no MISMATCH is printed. The run still exits 1 because
  of the build failure, but the dropout is not named until closedloop is
  fixed.

Before this fix, closedloop failures did not affect the count, because the
comparison was `Built` against `Expected`.

Fix: keep closedloop in its own list (or count it separately) so it adds to
the exit code but not to `-BuildFailures`. Alternatively, pass
`-BuildFailures` only the failures that `Invoke-HostTestExe` tracked. Add a
`check_host_test_verdict.ps1` case for it.

### LOW-2: the static call-site assertions do not rule out an exit-0 bypass elsewhere in the file (FIXED, devbreakfx12)

The new assertions pin the dot-source, the single `$verdict =` line with its
real counter arguments, and the three tail lines. They catch every edit to
those lines (C3-C7 below). They do not catch a bypass placed anywhere else.
Negtest against the check scored these six plausible edits MISSED:

- G1: `if ($script:failedExes.Count -gt 0) { exit 0 }` inserted before the
  verdict line.
- G2: `exit 0` in the outer `finally`. A `finally` exit overrides the `try`
  exit; this was confirmed in scratch, where `try { exit 1 } finally { exit 0 }`
  exits 0.
- G3: `$totalExpected` derived from the counters, so a dropout can never
  produce MISMATCH.
- G4: `Get-HostTestVerdict` redefined after the dot-source.
- G5: `$script:failedExes += $Name` removed.
- G6: the `-Only` no-match path exiting 0.

Real `build_host_tests.ps1 -Only main_reset_fence` runs with the
`s_phase_seen == 4` assertion flipped confirm the danger. The flip alone
(R0) exits 1. Adding G1 (R1) or G2 (R2) prints `FAIL ...:87` and
`RUN FAILURES (1)` but exits 0, and `check_host_test_verdict.ps1` passes on
both. R3 (the tail exit changed to `exit 0`) also exits 0, but the check does
catch it (C3).

This is a residual of REVIEW_DEVBREAKFX10 LOW-1, which is narrowed but not
closed. Cheap hardening:

- Count every `\bexit\b` statement in the file and require the known set.
- Refuse `function Get-HostTestVerdict` in `build_host_tests.ps1`.
- Require `$totalExpected = <integer literal>`.
- Require that the outer `finally` holds only `Exit-BuildLock`.
- Require exactly one append to each of `failedExes`/`builtExes`.

A behavioural test that runs the script against a deliberately failing
fixture would be stronger, but it costs a host build.

### INFO-1: dead branch in `Get-HostTestVerdict` (FIXED, devbreakfx12)

`host_test_verdict.ps1:18`, `if ($OnlyRun -and $Built -ne $Expected -and
$code -eq 0)`, can no longer fire. If `$code` is 0, then `$mismatch` is false
and `BuildFailures` is 0, which forces `Built == Expected`. It is harmless.
Delete it or keep it as a documented belt-and-braces line.

### INFO-2: stale header comment (FIXED, devbreakfx12)

`host_test_verdict.ps1:1-3` still says the MISMATCH check "runs whenever the
run is not -Only". Since INFO-2 it also runs under `-Only`.

### INFO-3: a dropout under `-Only` is invisible by construction

`$script:onlySelected` is filled inside `Invoke-HostTestExe` (line 573). An
executable whose call is never reached is therefore missing from both
`Expected` and `Built`. The new `-Only` MISMATCH catches only a selected
executable that later disappears between selection and the verdict. This is
expected; the full run is the dropout guard.

### INFO-4: LOW-3 rename is delete-then-move, not atomic

PS 5.1 `Move-Item -Force` deletes an existing target before moving. Since
line 126 removes the stale file first, the target never exists, and the
parent sees either no file or a complete one. No change is needed.

## Negative tests

All runs used `tools/negtest.ps1`. Both runs reported
`real_tree_unchanged=true` and `copies_removed=true`, and both baselines
passed.

`-Preset check -PresetArg check_host_test_verdict.ps1`:

| Mutation | Verdict | Failing assertion |
|---|---|---|
| C1 `$accounted = $Built` (revert LOW-2) | CAUGHT | FAIL (2): no-false-MISMATCH cases |
| C2 `(-not $OnlyRun) -and` restored (revert INFO-2) | CAUGHT | FAIL (1): dropout under -Only |
| C3 tail `exit 0` | CAUGHT | verdict ExitCode reaches exit before the success exit 0 |
| C4 `-OnlyRun $true` | CAUGHT | verdict assigned once, straight from Get-HostTestVerdict with the real counters |
| C5 `-RunFailures 0` | CAUGHT | same |
| C6 dot-source removed | CAUGHT | dot-sources host_test_verdict.ps1 exactly once |
| C7 `exit [int]$false` | CAUGHT | verdict ExitCode reaches exit |
| G1 early `exit 0` before verdict | MISSED | LOW-2 |
| G2 `exit 0` in finally | MISSED | LOW-2 |
| G3 self-derived `$totalExpected` | MISSED | LOW-2 |
| G4 function shadowed | MISSED | LOW-2 |
| G5 `failedExes` never appended | MISSED | LOW-2 |
| G6 `-Only` no-match exits 0 | MISSED | LOW-2 |

These are real runs of `build_host_tests.ps1 -OutDir {OUT} -Only
main_reset_fence`, judged by exit code. A CAUGHT here means exit 1:

| Mutation | Verdict | Notes |
|---|---|---|
| R0 flip `s_phase_seen == 4` -> `5` | CAUGHT | compiled; `FAIL test_main_reset_fence.c:87`, `RUN FAILURES (1)` |
| R1 flip + G1 | MISSED (exit 0) | FAIL printed, exit 0; check passes (LOW-2) |
| R2 flip + G2 | MISSED (exit 0) | FAIL printed, exit 0; check passes (LOW-2) |
| R3 flip + C3 | MISSED (exit 0) | the check catches it (C3) |
| R4 compile error in the selected test | CAUGHT | compile failure on purpose (Q2); no MISMATCH, exit 1 |
| R5 compile error in closedloop | CAUGHT | compile failure on purpose; false MISMATCH line (LOW-1) |

Every CAUGHT in the check table is an assertion failure in
`check_host_test_verdict.ps1`, not a parse error. R0 is a compiled test
assertion failure. R4 and R5 are intended compile failures, used to examine
the verdict text, not as evidence that an assertion works.
