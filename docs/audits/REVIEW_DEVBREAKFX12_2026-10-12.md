# Review: devbreakfx12 (b6dc626f6, f7bba4ffa) -- host-test verdict LOW-1/LOW-2/INFO-1/INFO-2 fixes

Reviewer: rvdevbreakfx12 (opus), 2026-10-12, base origin/dev 6c0eaf58f.

Result: **0 HIGH, 1 MED, 3 LOW, 5 INFO.** The functional fix is correct.
`sim_credibility_gate_closedloop` compile failures are now kept out of
`-BuildFailures`, and the exit is still nonzero, including under `-Only`.
The new AST assertions catch what they target. However, 13 of 13 bypass
mutants written for this review got past them (MISSED), and the commit
also landed 54 MB of build output.

## Scope

- `firmware/KilnFW/App/test/build_host_tests.ps1`: `$script:auxBuildFailures` (:515-517), the closedloop append (:3080), the AUX summary block and the aux `exit 1` (:3617-3624).
- `tools/host_test_verdict.ps1`: dead `-OnlyRun` branch removed, header fixed.
- `tools/check_host_test_verdict.ps1:39-64`: new AST and token assertions.
- `docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md`: follow-up row text.

## Answers to the review questions

**Q1. Can an aux build failure be silently ignored by anything?** Not by any
current caller. Every reader decides on the exit code:

- `check_00_kilnfw_host_tests.ps1:107-134` exits 1 on any nonzero exit. It greps the `Built:` line for display only.
- `tools/verify.ps1` and `tools/regression_suite.py` (`_gate_kilnfw_host_tests`) also use the exit code.
- `run_all_checks.ps1`, and `land.ps1` through its check log, see check_00's FAIL.
- The `negtest.ps1` kilnfw-host preset (`:701`, default pattern `RUN FAILURES \(`) scores an aux-only compile break as MISSED. That is correct, because a compile break is not an assertion. With the coordinator's `BUILD FAILURES` ExpectPattern it scores CAUGHT, because `AUX BUILD FAILURES (` contains that text. That falls under the same compile-failure caveat as every other build break.

The closedloop build is outside the `-Only` filter (:3056-3083), so it is
compiled and gated on every `-Only` run. This was verified behaviourally with
negtest, using a custom command that runs `build_host_tests.ps1 -Only
"test_update_stage\.c"` with a `#error` mutant in
`sim_credibility_gate_closedloop.c`:

- Baseline PASS.
- Mutant CAUGHT, exit 1, with `Built: 1/1 executables`, `AUX BUILD FAILURES (1)`, `BHT_EXIT=1` and no MISMATCH.
- The real tree was unchanged.

Two residual paths are pre-existing and are not new to this change (INFO-5):

- `-Fast` skips check_00.
- `-AllowKnownFailures`/`-FailOnlyOnNew` work per check. A new aux break hides if check_00 already fails on main.

**Q2. Can the AST assertions be bypassed?** Yes. All 13 bypass mutants below
exit 0 under the check (MISSED). The token scan (`:44-46`) does catch every
added `exit` keyword, including inside a function, scriptblock, trap or
finally (control C1 CAUGHT). Termination without the `exit` keyword,
counter rewrites, and the text-only aux assertion are not covered. See
LOW-1, LOW-2, LOW-3 and INFO-1.

**Q3. Is the assertion set brittle?** Mildly. See INFO-2. The `'99'` literal
(`:55`) adds a second place that must change on every new host test. Every
assertion that pins an exact text line (`:36`, `:45`, `:47`) breaks on a
reformat. Their failure messages print what was found, so updating them is
quick.

**Q4. Is Built+BuildFailures==Expected still exact when closedloop fails?**
Yes. `build_host_tests.ps1` has exactly 99 `Invoke-HostTestExe` call sites,
and `$totalExpected = 99` (:3588). Closedloop is not built through
`Invoke-HostTestExe` and is no longer in `$script:buildFailures`. Under
`-Only`, `$totalExpected` is `$script:onlySelected.Count`, and closedloop is
never selected. The `-Only` negtest above showed `Built: 1/1` with no false
MISMATCH while closedloop failed.

## Negative tests

All runs used `tools\negtest.ps1 -Preset check -PresetArg
tools\check_host_test_verdict.ps1 -ExpectPattern '(?m)^\s+FAIL: '`, so
CAUGHT requires a failing named assertion. The mutation JSON was kept in
private scratch. Baseline PASS; real tree unchanged; copies removed.

All mutants target `build_host_tests.ps1`.

| Mutant | Edit | Verdict |
|---|---|---|
| B1 | closedloop `auxBuildFailures +=` line commented out | **MISSED** (LOW-1) |
| B2 | `if (aux) { [Environment]::Exit(0) }` before the summary | **MISSED** (INFO-1) |
| B3 | `if (aux) { break }` at script scope | **MISSED** (LOW-3) |
| B4 | `if (aux) { return }` at script scope | **MISSED** (LOW-3) |
| B5 | `Set-Alias Get-HostTestVerdict Fake-Verdict` (alias beats function) | **MISSED** (INFO-1) |
| B6 | `function global:Get-HostTestVerdict {...}` | **MISSED** (INFO-1) |
| B7 | `${function:Get-HostTestVerdict} = {...}` | **MISSED** (INFO-1) |
| B8 | `$script:failedExes = @()` before the summary | **MISSED** (LOW-2) |
| B9 | `$script:auxBuildFailures = @()` before the summary | **MISSED** (LOW-2) |
| B10 | `$failedExes = @()` (unqualified, same script-scope variable) | **MISSED** (LOW-2) |
| B11 | `$script:totalExpected = builtExes + buildFailures` | **MISSED** (LOW-2) |
| B12 | outer `try` gains `catch { }` (a thrown build error exits 0) | **MISSED** (LOW-3) |
| B13 | aux append moved from the failure branch to the `else` branch | **MISSED** (LOW-1) |
| C1 | `exit 0` added inside `Invoke-HostTestRun` | CAUGHT: `exactly the expected exit statements` |
| C2 | closedloop back in `$script:buildFailures` | CAUGHT: both LOW-1 assertions |
| C3 | aux `exit 1` line deleted | CAUGHT: `verdict ExitCode reaches exit`, exit-set |
| C4 | `function Get-HostTestVerdict {...}` added | CAUGHT: `is not redefined` |
| C5 | `$totalExpected = builtExes + buildFailures` | CAUGHT: `$totalExpected is only the literal 99` |

B2-B4, B5 and B12 were also confirmed with standalone scripts in
`powershell -File` mode:

- `break`, `return`, `[Environment]::Exit(0)`, an alias shadowing a function, and `catch {}` around a throw each give exit 0.
- `$host.SetShouldExit(0)` followed by `exit 1` gives 1, so it is not a bypass while an `exit` follows.

There was also one behavioural negtest: the closedloop `#error` under `-Only`
(see Q1), which was CAUGHT, exit 1.

## Findings

### MED-1: b6dc626f6 committed 1406 build artifacts (54 MB) to dev

`git show --stat b6dc626f6` adds `wtdevbreakfx12_scratchout/`: 1003 `.obj`,
104 `.exe`, 297 `.txt` and 2 `.rsp` files, about 54 MB. It looks like a
host-test `-OutDir` at the repo root, picked up by a broad `git add`. The
files were still present at origin/dev 6c0eaf58f. The squash promote would
carry them to main, and every clone pays for them.

**Fixed in this landing:**

- `git rm -r wtdevbreakfx12_scratchout`.
- `.gitignore` gains `/*_scratchout/`, next to the existing `/*.obj` rule.

Dev history keeps the blobs, but a squash promote after this commit will not.

### LOW-1: the closedloop-to-aux assertion is raw text, satisfied by a comment or the wrong branch

`check_host_test_verdict.ps1:64` is `$src -match '\$script:auxBuildFailures \+= "sim_credibility_gate_closedloop"'`.
That regex matches comment text, and it does not care which branch the
append sits in.

- B1 (append commented out) is MISSED, so a closedloop compile break is silently ignored. This is the exact regression LOW-1 of DEVBREAKFX11 fixed.
- B13 (append moved to the success branch) is MISSED as well.

Assertion `:63` only proves that the append is *not* in `buildFailures`.

Fix: use the AST. Require exactly one `AssignmentStatementAst` with left
`$script:auxBuildFailures`, operator `PlusEquals` and right
`"sim_credibility_gate_closedloop"`. Also require that its nearest
`IfStatementAst` ancestor's first clause condition contains `$LASTEXITCODE
-ne 0` and that the append sits in that clause body, not in `ElseClause`.

### LOW-2: the counter lists and `$totalExpected` can be rewritten without tripping anything

`:56-61` count only `+=` appends to `failedExes`/`builtExes`, and `:54`
matches `$totalExpected` by exact left-hand text. None of these are pinned:

- a plain re-assignment `$script:failedExes = @()` (B8) or `$script:auxBuildFailures = @()` (B9);
- an unqualified `$failedExes` at script scope, which is the same variable as `$script:failedExes` (B10);
- `$script:totalExpected` (B11).

Each one turns a red run green. B8 and B10 drop run failures, B9 drops aux
failures, and B11 hides dropouts. A misplaced reset is a plausible
refactoring accident, for example re-initialising the lists after the
`-Only` block.

Fix: match on `VariableExpressionAst.VariablePath.UserPath`, with any
`script:` prefix stripped and compared case-insensitively. Require each of:

- `builtExes`, `buildFailures`, `failedExes`, `auxBuildFailures` and `onlySelected` has exactly one `=` assignment (the `@()` initialiser);
- `totalExpected` has exactly the two known assignments;
- no `Set-Variable`/`New-Variable`/`Clear-Variable` names any of them.

### LOW-3: script-scope `break`/`return`/`continue` and a `catch` on the outer `try` exit 0

The exit-set assertion (`:44-46`) scans for the `exit` keyword only:

- A `break` or `return` outside any loop or function terminates the script with exit code 0 (B3, B4).
- `continue` behaves the same way.
- A `catch { }` added to the outer `try` (`build_host_tests.ps1:52`/`:3628`) turns any thrown error into exit 0 (B12). Examples are a gate failure, a missing `cl`, or an `$ErrorActionPreference = "Stop"` stderr line.

The outer-finally assertion (`:49-51`) does not look at catch clauses. An
early `return` in a new `-Only` path, or a `catch { Write-Host $_ }` added
for diagnostics, are realistic edits.

Fix: assert that the outer `TryStatementAst` has `CatchClauses.Count -eq 0`
and that the file has no `trap`. Assert that every `BreakStatementAst`/
`ContinueStatementAst` has a loop or `switch` ancestor inside the same
function, and that every `ReturnStatementAst` has a
`FunctionDefinitionAst`/`ScriptBlockExpressionAst` ancestor below the file
root.

### INFO-1: deliberate shadowing and non-keyword exits are out of reach

The following bypasses are MISSED, and none of them can be written by
accident:

- `[Environment]::Exit(0)` (B2).
- `Set-Alias Get-HostTestVerdict ...`, because aliases resolve before functions (B5).
- `function global:Get-HostTestVerdict` (B6). `FunctionDefinitionAst.Name` carries the `global:` prefix, so `-eq 'Get-HostTestVerdict'` misses it.
- `${function:Get-HostTestVerdict} = {...}` (B7).

Cheap partial hardening:

- reject `[Environment]::Exit`/`[System.Environment]::Exit` member invocations, `Set-Alias`/`New-Alias` commands and any `function:` drive variable;
- strip a scope prefix before comparing function names.

The fixer's own note is correct: a behavioural fixture is the only complete
answer, and it costs a host build.

### INFO-2: brittleness against legitimate edits

- `:55` pins `$totalExpected` to the literal `'99'`. Every new host test now needs `build_host_tests.ps1:3588` and this check edited together. Suggest asserting `^\d+$` for the literal, which keeps the "not self-derived" property and drops the second edit.
- `:36`, `:45` and `:47` pin exact source lines, including the blank line before the success `Write-Host` and the order of the exits. Any reformat fails. This is acceptable, because the messages print the lines found.
- `:49` uses `$ast.Find(...)`, which returns the first `try`-with-`finally` in pre-order. That is the outer one today, but a `try/finally` added above line 52 (for example around vcvars import) would silently retarget the assertion. Selecting the try whose parent is the script's top-level `NamedBlockAst` would be more robust.

### INFO-3: stale comments around the closedloop block

Two comments no longer match the code:

- `build_host_tests.ps1:3051-3052` still says to "never let its build/run result affect this script's own exit code". That has been false since 2026-09-10, and the comment at :3063-3077 contradicts it.
- `:3075-3077` says the failure goes "via the same `$script:buildFailures` list". It now goes via `$script:auxBuildFailures`.

### INFO-4: `-OnlyRun` is now an unused parameter

With the dead branch removed (INFO-1 of DEVBREAKFX11), `Get-HostTestVerdict`
(`host_test_verdict.ps1:6`) no longer reads `$OnlyRun`, but the call site
(`build_host_tests.ps1:3621`) and `check_host_test_verdict.ps1:33` still pin
it. It is harmless. Drop it in both places, or keep it with a one-line
comment saying it is informational.

### INFO-5: pre-existing masking paths, and `-Only` cannot select closedloop

- `run_all_checks.ps1 -Fast` skips `check_00_kilnfw_host_tests.ps1` entirely. `-FailOnlyOnNew` and `land.ps1 -AllowKnownFailures` judge per check, so a new aux break inside an already KNOWN-failing check_00 is not reported as NEW. This is the general granularity limit of the baseline mechanism, not specific to this change.
- `-Only sim_credibility_gate_closedloop` matches no `Invoke-HostTestExe` item, so it always exits 1 with "matched no host-test executable", even when the aux build succeeds. That fails loud, but it is not a useful way to iterate on that file. Every `-Only` run also pays for the closedloop compile.

## Verdict

LOW-1 and INFO-1/INFO-2 of DEVBREAKFX11 are fixed as claimed. LOW-2 of
DEVBREAKFX11 is only partly closed: the `exit`-keyword surface is covered,
but the termination paths and counter rewrites above (this review's
LOW-1..LOW-3) remain. MED-1 is fixed in the same landing as this review.
