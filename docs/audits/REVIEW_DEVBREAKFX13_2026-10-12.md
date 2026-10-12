# Review: devbreakfx13 (e26b99909, 2ff94f600) -- host-test verdict AST pins

Reviewer: rvdevbreakfx13 (opus), 2026-10-12, base origin/dev 6d75ddfeb.

Result: **0 HIGH, 1 MED, 4 LOW, 6 INFO.** The fix does what REVIEW_DEVBREAKFX12 asked:

- The closedloop aux append is pinned structurally, inside the failure clause.
- The four counters are pinned by name, with any scope prefix stripped.
- Catch, trap, script-scope break/continue/return, alias shadowing and `[Environment]::Exit` are refused.
- The dead `-OnlyRun` parameter is gone, and nothing calls it any more.

All four control mutants were CAUGHT, each with a named `FAIL:` line. However,
15 of 15 bypass mutants were MISSED. The most important gap is MED-1: the
check pins how the counters are wired into the verdict, but not the code that
decides a test run failed. A one-token edit there, or in the dot-sourced
`tools/host_test_exec.ps1`, hides every run failure, and no standing check
would notice.

## Scope

- `tools/check_host_test_verdict.ps1`: the new AST section (lines 39-125 at e26b99909).
- `tools/host_test_verdict.ps1`: `-OnlyRun` removed.
- `firmware/KilnFW/App/test/build_host_tests.ps1`: comment fixes and the call site without `-OnlyRun`.
- `docs/audits/REVIEW_DEVBREAKFX12_2026-10-12.md` and `GATE_NEGATIVE_TEST_EVIDENCE.md`: status text.

## Findings

### MED-1: the run-failure producer is not pinned or tested

`Invoke-HostTestRun` (`build_host_tests.ps1:548-564`) appends to
`$script:failedExes` only `if ($exeExit -ne 0)`. `$exeExit` comes from
`Invoke-HostTestProcess` in `tools/host_test_exec.ps1`, which returns
`$p.ExitCode`. The check pins that there is exactly one `failedExes` append
and that it appends `$Name`. It does not pin the condition around the append,
and it never looks at `host_test_exec.ps1`. No other check covers that file
either: `git grep` finds only the two `build_host_tests.ps1` scripts that
dot-source it, and the SaftyFW script is one of them.

The two MISSED mutants:

- **B09**: `if ($exeExit -ne 0)` becomes `-lt 0`.
- **B10**: `return $p.ExitCode` becomes `return 0`.

Either one makes every failing host test report green. `check_00_kilnfw_host_tests.ps1`
runs a suite that passes, so it cannot see the change. The `kilnfw-host` negtest
preset would see it, but only when someone runs it. The same applies to SaftyFW,
which uses the same runner.

Suggested fix:

1. Add a behavioural unit test of `Invoke-HostTestProcess`. Run `cmd /c exit 3` and expect 3, run an exit-0 command and expect 0, and run a short timeout and expect 124.
2. AST-pin the `-ne 0` condition that encloses the `failedExes` append, the same way the closedloop `$LASTEXITCODE -ne 0` clause is now pinned.

### LOW-1: dot-sourced libraries are outside the check

`build_host_tests.ps1:43-46` dot-sources `build_lock.ps1`, `host_test_exec.ps1`,
`build_gate.ps1` and `host_test_verdict.ps1`, and functions from all four run
in the script's scope. The check scans only `build_host_tests.ps1`. It never
scans the libraries, and it does not pin the set of dot-sources. The bypasses
this leaves:

- **B11, `exit 0` inside `Exit-BuildLock`.** The outer `finally` calls `Exit-BuildLock`, and an `exit` in a function called from a `finally` overrides the earlier `exit $verdict.ExitCode`. This was checked empirically: `try { exit 1 } finally { Rel }` with `function Rel { exit 0 }` exits 0.
- **B12, an extra function in `host_test_verdict.ps1`.** It is dot-sourced last, so a `function Invoke-HostTestProcess { 0 }` defined there silently replaces the real runner. The check dot-sources the same file, but it only unit-tests `Get-HostTestVerdict`.
- **B13, a second dot-source after the verdict file.** Any later dot-source, here a re-dot-source of `host_test_exec.ps1` carrying a fake `Get-HostTestVerdict`, wins. The "dot-sourced exactly once" pin matches only the verdict path.

A top-level `exit 0` in a dot-sourced file is not a bypass. A dot-sourced
script's `exit` ends only that file (`. lib.ps1; exit 1` still exits 1), so it
was not scored.

Suggested fix:

1. Pin the exact ordered list of dot-source statements.
2. Run the Exit-token, `[Environment]::Exit` and function-name scans over each dot-sourced library too.
3. Refuse any function in a library that shares a name with a function defined in another library or in `build_host_tests.ps1`.

### LOW-2: dynamic evaluation and the variable/function providers bypass the deny-list

The check refuses specific syntax, so any equivalent written another way gets
through. All of the following were MISSED:

| Mutant | Construct |
|---|---|
| B01 | `Invoke-Expression 'exit 0'` |
| B02 | `& ([scriptblock]::Create('exit 0'))` |
| B04 | `$ExecutionContext.SessionState.PSVariable.Set('failedExes', @())` |
| B05 | `(Get-Variable failedExes).Value = @()` |
| B06 | `${variable:failedExes} = @()` (`Norm-Name` strips only scope prefixes, not the `variable:` drive) |
| B07 | `Remove-Item Variable:failedExes` |
| B08 | `Set-Item function:Get-HostTestVerdict -Value { ... ExitCode = 0 }` (the path is a string, so the `function:` VariableExpression scan misses it) |

None of these constructs appears in `build_host_tests.ps1` today, so the
following bans can be added without changing the script:

- `Invoke-Expression`/`iex`;
- any `::Create` on `[scriptblock]`;
- any use of `$ExecutionContext`;
- `Get-Variable`, `Set-Item`, `New-Item`, `Remove-Item`, `Rename-Item` and `Copy-Item` whose path argument starts with `variable:`, `function:` or `alias:`;
- any VariableExpression whose `UserPath` names a drive other than a scope prefix.

The deny-list approach can never be complete. MED-1's behavioural test is
the more durable guard.

### LOW-3: a `break` in a function, called outside a loop, ends the script with exit 0

The LOW-3 rule accepts a `break`/`continue` that has a function ancestor.
PowerShell break is dynamically scoped, though. A `break` in a function that
was called outside any loop ends the whole script with exit 0. This was
checked empirically: `function F { break }; try { F; exit 1 } finally { }`
exits 0.

The same `break`, in a helper called from inside `Complete-HostTestQueue`'s
`foreach`, would instead break that caller's loop.

**B03** (`function Stop-Early { break }; Stop-Early`) was MISSED.

Suggested fix: walk up from each `break`/`continue` and require a loop or
switch before reaching a function boundary. All three existing break/continue
statements (:538, :539, :625) already satisfy that rule, so tightening it is
free.

### LOW-4: a nested try/catch swallows a throw and falls through to exit 0

The check refuses a catch on the outer try only. If the body inside the outer
try is wrapped in `try { ... } catch { }`, a throw skips the verdict tail. The
script then runs off the end of the outer try and exits 0. This was checked
empirically: `try { try { throw; exit 1 } catch { } } finally { }` exits 0.

| Mutant | Edit | Result |
|---|---|---|
| B14 | `catch { }` on a nested try around the whole body | MISSED |
| B15 | `catch { $exeExit = 0 }` in `Invoke-HostTestRun` | MISSED |

B15 matters only for thrown errors, because a crashing test exe still returns
a nonzero code. Today there is no catch clause anywhere in the file, so
"no catch clause anywhere" is a free, stricter rule.

### INFO-1: how the outer try is selected

`Parent -is NamedBlockAst` also matches the try/finally in
`Invoke-HostTestRun` (:556), because a function body is a NamedBlockAst too.
The AST has exactly two such trys, at :52 and :556. Uniqueness therefore
comes from the `Exit-BuildLock` text filter, and that filter fails closed: a
count other than 1 is a FAIL. Selecting with `$n.Parent -eq $ast.EndBlock`
would say what is meant. This is not exploitable today, because wrapping the
body in a function keeps the same semantics.

### INFO-2: `-OnlyRun` callers

`git grep "OnlyRun|Get-HostTestVerdict"` across the whole repo, excluding
`docs/`, finds only the call site (:3620) and the definition. Nothing under
`tools/`, `firmware/` or `.claude/` still passes `-OnlyRun`. Two case labels
in the check still say "under -Only" ("build failure under -Only", "dropout
under -Only (INFO-2)"), although they are now plain counter cases. That is
cosmetic.

### INFO-3: check_source_path_drift fails on dev (noted only, not fixed)

Running `firmware/KilnFW/App/test/check_source_path_drift.ps1` at
6d75ddfeb gives `SOURCE PATH DRIFT CHECK: FAILED` with this line:

```
tools/check_host_test_verdict.ps1:31: Join-Path references '\.\./\.\./\.\./\.\./tools/host_test_verdict\.ps1' which does not exist
```

The drift checker reads the `Join-Path` inside the check's regex literal (the
dot-source pin) as a real path. devbreakfx14 owns this fix.

### INFO-4: patterns from the brief that are not bypasses

- **Splatting.** The call line is pinned verbatim, so a splat fails `callRe`. `$PSDefaultParameterValues` fills only unspecified parameters, and all four are given.
- **A second script block resetting `$LASTEXITCODE`.** The verdict reads only the counters. The closedloop gate is `$LASTEXITCODE -ne 0 -or -not (Test-Path $exeClosedloop)`, and the exe is deleted before the compile (:3062), so a compile break still trips `Test-Path`.
- **A pipeline `Write-Output` of a fake verdict line.** Every consumer decides on the exit code: `check_00_kilnfw_host_tests.ps1:107-126`, `verify.ps1`, `regression_suite.py` and workbench. The text alone cannot turn a run green.
- **`exit` in a nested function.** The Exit-token list covers every `exit` token in the file in order, wherever it appears, so it is caught by construction. C1 shows the rule firing.

### INFO-5: `host_test_verdict.ps1` behaviour is covered only at 0/1

The unit cases use `RunFailures` and `BuildFailures` values of 0 and 1 only.
An edit such as `-eq 1` in place of `-gt 0` would still pass. Adding one case
with `RunFailures 5` and one with `BuildFailures 3` costs nothing.

### INFO-6: file hygiene

e26b99909 added a UTF-8 BOM to `tools/check_host_test_verdict.ps1` and dropped
its trailing newline. PowerShell 5.1 handles both. It is churn only.

## Negative test

Command:

```
tools\negtest.ps1 -Preset check -PresetArg tools/check_host_test_verdict.ps1 -Mutations <scratch>/mutations.json -ExpectPattern "(?m)^\s+FAIL: "
```

The mutation JSON was kept in a private scratch directory. Two runs were made
from base 6d75ddfeb:

| Run | Mutants | Log dir | Baseline | `real_tree_unchanged` |
|---|---|---|---|---|
| 1 | 16 | `negtest_logs\20261012_091643_san9` | PASS, 15.3 s | true |
| 2 | 3 | -- | PASS | true |

Every CAUGHT below has a named `FAIL:` line.

| # | Mutant | Verdict | Matched FAIL / note |
|---|---|---|---|
| C1 | `exit 0;` before the summary | CAUGHT | `FAIL: exactly the expected exit statements, in order` |
| C2 | `$script:failedExes = @();` reset | CAUGHT | `FAIL: exactly one initializer (= @()) of $failedExes, no resets (got 2)` |
| C3 | top-level `break;` | CAUGHT | `FAIL: every break/continue has a loop, switch or function ancestor (LOW-3) (bad: 3596)` |
| C4 | `catch { }` on the outer try | CAUGHT | `FAIL: outer try has no catch clause ...` |
| B01 | `Invoke-Expression 'exit 0'` | MISSED | LOW-2 |
| B02 | `& ([scriptblock]::Create('exit 0'))` | MISSED | LOW-2 |
| B03 | `function Stop-Early { break }; Stop-Early` | MISSED | LOW-3 |
| B04 | `$ExecutionContext...PSVariable.Set('failedExes', @())` | MISSED | LOW-2 |
| B05 | `(Get-Variable failedExes).Value = @()` | MISSED | LOW-2 |
| B06 | `${variable:failedExes} = @()` | MISSED | LOW-2 |
| B07 | `Remove-Item Variable:failedExes` | MISSED | LOW-2 |
| B08 | `Set-Item function:Get-HostTestVerdict -Value {...}` | MISSED | LOW-2 |
| B09 | `if ($exeExit -ne 0)` becomes `-lt 0` | MISSED | MED-1 |
| B10 | `host_test_exec.ps1`: `return $p.ExitCode` becomes `return 0` | MISSED | MED-1 |
| B11 | `build_lock.ps1`: `exit 0` in `Exit-BuildLock` | MISSED | LOW-1 |
| B12 | `host_test_verdict.ps1` defines `Invoke-HostTestProcess { 0 }` | MISSED | LOW-1 |
| B13 | second dot-source of `host_test_exec.ps1` carrying a fake `Get-HostTestVerdict` | MISSED | LOW-1 |
| B14 | nested `try { body } catch { }` inside the outer try | MISSED | LOW-4 |
| B15 | `catch { $exeExit = 0 }` in `Invoke-HostTestRun` | MISSED | LOW-4 |

The runtime effect of B03, B11, B14 and the dot-sourced `exit` (not a bypass)
was confirmed with small standalone scripts. Each scored mutant leaves at least
one host-test failure path ending in exit 0.
