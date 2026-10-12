# Review: DEVBREAKFX10 (`a6e5bd18f` + `4d4ac698d`), 2026-10-12

Adversarial review of the REVIEW_DEVBREAKFX8 LOW-1/LOW-2/INFO-5 fixes on
origin/dev:

- `a6e5bd18f`: new `tools/host_test_verdict.ps1` (`Get-HostTestVerdict`),
  dot-sourced by `firmware/KilnFW/App/test/build_host_tests.ps1`; new
  `tools/check_host_test_verdict.ps1`; and the
  `tools/check_build_gate_reentrant.ps1` case-5 handshake change.
- `4d4ac698d`: audit doc status only.

Reviewed at dev tip `c6689c6ae` in a worktree minted from origin/dev. Line
numbers are at that tip.

**Verdict: no HIGH, no MED. 3 LOW, 4 INFO.** The fix does what it says. A run
failure no longer masks a dropout. The exit code is still 0 or 1, and the
verdict lines that consumers match on are unchanged. The gap: nothing checks
the call site in `build_host_tests.ps1`, so an edit there could turn a real
FAIL into exit 0 and no check would catch it (LOW-1).

## Evidence

- **Full run.** `build_host_tests.ps1` was run at the tip with no `-Only` and
  `-OutDir` in scratch. Result: `Built: 99/99 executables`,
  `RUN FAILURES (1): autotune_engine_prestart`, no MISMATCH line, exit 1.
  This is the known dev-red failure. It is still reported, and no MISMATCH
  appears where none is due.
- **`-Only` with a typo.** `-Only test_no_such_thing_xyz` printed
  `-Only 'test_no_such_thing_xyz' matched no host-test executable` and exited
  1. Lines 3586-3590 are untouched by the diff.
- **`check_host_test_verdict.ps1` run directly:** PASS, exit 0.
  **`check_build_gate_reentrant.ps1` run directly:** PASS, exit 0. Case 5
  logged `ready file says 'got=True'`.
- **Discovery.** `Get-KilnCheckScripts` (`tools/lib_check_discovery.ps1`, used
  by `run_all_checks.ps1`) lists both `check_host_test_verdict.ps1` and
  `check_build_gate_reentrant.ps1`.

### Negative tests

All runs used `tools/negtest.ps1` in throwaway copies. Every run reported
`real_tree_unchanged: true` and `copies_removed: true`.

**Run 1:** `-Preset check -PresetArg tools\check_host_test_verdict.ps1`.
Baseline PASS.

| Mutation | Verdict | Evidence |
|---|---|---|
| A: `host_test_verdict.ps1:15`, drop `-or $mismatch` from the exit-code condition | CAUGHT | real assertion: `FAIL: dropout alone: MISMATCH and nonzero` |
| A2: `host_test_verdict.ps1:7`, MISMATCH only when there are no failures (the pre-fix LOW-1 shape) | CAUGHT | real assertion: `FAIL: dropout while a test fails: MISMATCH still reported, nonzero` |
| B: `build_host_tests.ps1:46`, delete the dot-source of `host_test_verdict.ps1` | MISSED | the check never loads `build_host_tests.ps1` |
| C: `build_host_tests.ps1:3616`, replace `if ($verdict.ExitCode -ne 0) { exit ... }` with `if ($false) { }` | MISSED | same reason |

**Run 2:** `-Command "build_host_tests.ps1 -Only main_reset_fence -OutDir {OUT}"`.
No `-ExpectPattern`, so CAUGHT means a nonzero exit. Baseline PASS.

| Mutation | Verdict | Evidence |
|---|---|---|
| D (control): `test_main_reset_fence.c:104`, flip `!s_hook_kv("nvs")` | CAUGHT, exit 1 | real assertion: `FAIL ...test_main_reset_fence.c:104: other partition not refused`; the test compiled (`Built: 1/1`) |
| E: D plus mutation C at the call site | MISSED, **exit 0** | log shows the same `FAIL ...:104`, then `RUN FAILURES (1):`, then `all 1 host test executables built and passed` |
| F: D plus `$verdict` forced to `ExitCode=0` at line 3614 | MISSED, **exit 0** | same as E |

## Findings

### LOW-1: no check covers the call site in `build_host_tests.ps1`

`build_host_tests.ps1:3614-3616`. `check_host_test_verdict.ps1` tests only
the pure function. Run 1's B and C show it is blind to the wiring. B fails
closed: under `$ErrorActionPreference = "Stop"`, an undefined
`Get-HostTestVerdict` throws inside `try`, so every run goes red. C, E and F
do not fail closed. A one-line edit at line 3616, or to the `$verdict`
assignment at 3614, makes a run with a real `FAIL file:line` and
`RUN FAILURES (1)` exit 0 and print "built and passed".

These consumers judge the run by exit code alone:

- `check_00_kilnfw_host_tests.ps1`, and through it `run_all_checks.ps1` and
  `land.ps1`
- `regression_suite.py`

All of them would report PASS. The old inline code had the same exposure;
moving the logic into a function did not add a test for it. The kilnfw-host
negtest recipe cannot catch this either (see INFO-1).

Fix: add a wiring case to `check_host_test_verdict.ps1`. One way is to
static-assert that `build_host_tests.ps1` contains exactly one dot-source of
`host_test_verdict.ps1`, a `Get-HostTestVerdict` call, and
`exit $verdict.ExitCode` guarded by `-ne 0`, all before the success `exit 0`.
A more robust way is to factor the summary into a function that a fake run
can call.

### LOW-2: MISMATCH text appears on every build failure and says it is not a build failure

`host_test_verdict.ps1:7-12`. An executable whose compile fails goes into
`$script:buildFailures`, never into `$script:builtExes` (`Complete-HostTestQueue`,
`build_host_tests.ps1:615-623`; serial path :582-587). So on any non-`-Only`
run with a build failure, `Built < Expected`. That now prints:

`MISMATCH: 98 executables built but 99 were expected -- at least one was
silently never attempted (not a build failure, not a run failure)`

This sits right under `BUILD FAILURES (1)`. The text is false. It also
conflates the two cases: one build failure plus one real dropout reads
`97 ... 99`, and the reader has to do the arithmetic to see the dropout.
Before the fix, the build-failure path exited before the MISMATCH check, so
this text never appeared. The exit code is right either way, and the text
does not match the negtest ExpectPattern, so no verdict changes.
`check_host_test_verdict.ps1` has no non-`-Only` case with
`BuildFailures > 0`.

Fix: compute `$mismatch = (-not $OnlyRun) -and (($Built + $BuildFailures) -ne $Expected)`,
and add that case to the check.

### LOW-3: case-5 ready-file read can race the child's write (spurious red)

`check_build_gate_reentrant.ps1:56,129,133`. The parent polls
`Test-Path $ready`, then reads `(Get-Content $ready -Raw).Trim()`. The child
writes with `Set-Content`, which creates the file before it writes the bytes.
If the parent reads in that window, it gets an empty file. In Windows
PowerShell 5.1, which the check runs under, `Get-Content -Raw` on an empty
file returns `$null`, and `.Trim()` throws "You cannot call a method on a
null-valued expression" (reproduced here on 5.1.26100). The outer `catch`
then files it as `FAIL: unexpected error`. The window is microseconds and
the result is red, never a false green, but it is a flake source in a
standing check.

Fix: have the child write a temp file and `Move-Item` it onto `$ready`, which
is an atomic rename. Or have the parent poll until the content is non-empty,
with a deadline, and cast with `"$(...)".Trim()`.

### INFO-1: the standard kilnfw-host negtest recipe cannot see exit-code regressions

The documented ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`, and the preset's
own `RUN FAILURES \(`, match text and ignore the exit code (negtest header
lines 34-38). In Run 2's E and F, that pattern matches (`matched` lists the
FAIL line and `RUN FAILURES (1):`) while the script exits 0. So a negtest run
with this recipe scores an exit-code-masking mutant CAUGHT.

Also, with the dev tip red on `autotune_engine_prestart`, a full-run
kilnfw-host baseline fails. Any wiring test of `build_host_tests.ps1` today
needs an `-Only` command, as in Run 2.

### INFO-2: `-Only` dropout now exits 1 with no explanation

`host_test_verdict.ps1:16`. Under `-Only`, `Mismatch` is false, so no text
prints, but `Built != Expected` still forces exit 1. Before the fix, this
case printed the MISMATCH line. Now the only clue is `Built: x/y`. It is
reachable only if a selected executable is queued but never reaches
`Invoke-HostTestRun` or a build failure. That is rare, and the exit code is
right. A one-line `Write-Host` would restore the explanation.

### INFO-3: answers to the questions asked

1. **Exit code and verdict lines.**
   - The exit code is still 0 or 1.
   - `Built:`, `BUILD FAILURES (`, `RUN FAILURES (` and
     `all N host test executables built and passed` are unchanged.
   - The new MISMATCH text contains no `FAIL `, so it cannot cause a false
     CAUGHT under the kilnfw-host ExpectPattern.
   - A real FAIL is not masked, and a pass does not turn red. `Built == Expected`
     with no failures gives exit 0. The full run's exit 1 is the known
     `autotune_engine_prestart` failure.
   - `verify.ps1`'s decisive regex `^\s*FAIL |FAILURE\(S\)|...` is unaffected.
2. **`-Only`.** The path is unchanged, and a typo still fails loud with
   exit 1. The INFO-2 text loss is the only difference.
3. **Wiring.** `check_host_test_verdict.ps1` is discovered by `run_all_checks`
   and is not vacuous for the function (A and A2 are CAUGHT on real
   assertions). It is vacuous for the wiring (LOW-1).
4. **The fixer's mutation.** It did not exercise `build_host_tests.ps1`.
   - Removing the dot-source fails closed (red), so it is not dangerous.
   - Ignoring the function's result is not caught, and it turns a red run
     green by exit code (E, F).

### INFO-4: LOW-2 and INFO-5 of DEVBREAKFX8 are correctly fixed

- The child now writes `got=$got` before the `exit 2` path, so a child that
  fails to acquire leaves `got=False` and the parent's content assertion
  fails.
- `Remove-Item $ready` before `Start-Child` removes the stale-file vacuous
  pass.
- The content assertion is the stronger handshake the DEVBREAKFX8 review
  asked for.

## Fix status

- LOW-1 fixed (8416ac8c7): check_host_test_verdict.ps1 statically asserts the dot-source, the single verdict assignment with the real counters, and ExitCode reaching exit before the success exit 0; mutations B, C, F CAUGHT on real assertions.
- LOW-2 fixed (8416ac8c7): MISMATCH compares Built + BuildFailures to Expected; check cases added.
- LOW-3 fixed (8416ac8c7): child writes the ready file via temp + Move-Item; parent read is null-safe. (Race itself is not deterministically negtestable.)
- INFO-2 fixed (8416ac8c7): -Only dropout now prints the MISMATCH line.
