# Review: devbreakfx15 (60e26264b, af337ce9e) -- host-test verdict check, REVIEW_DEVBREAKFX13 fixes

Reviewer: rvdevbreakfx15 (opus), 2026-10-12, base origin/dev 713c474d8.

Result: **0 HIGH, 2 MED, 3 LOW, 4 INFO.**

The fix closes what REVIEW_DEVBREAKFX13 scored. I re-ran four of its mutants as
controls (B09, B10, B03, and a library `exit 0`), and all four were CAUGHT with
a named `FAIL:` line. Its author scored the other fifteen B mutants. The check
now does the following:

- It runs the real `Invoke-HostTestProcess` against a process that exits 3, one that exits 0, and a 1 s timeout.
- It pins the `if ($exeExit -ne 0)` guard.
- It pins the dot-source list and scans the four libraries.
- It refuses the literal dynamic-eval forms.
- It applies the loop-before-function break rule.
- It allows no catch clause anywhere.

The new gaps sit one step beside the fixed ones, and 13 of 14 new bypass mutants
were MISSED. Two matter most. A `break` inside a library function ends both
`build_host_tests.ps1` and this check with exit 0 (MED-1). The `$exeExit` and
`$ExePath` pins key on raw text, so an equivalent spelling slips past them (MED-2).

## Scope

- `tools/check_host_test_verdict.ps1`: lines 127-200 are new or changed in 60e26264b, plus INFO-1/2/6 cosmetics.
- `docs/audits/REVIEW_DEVBREAKFX13_2026-10-12.md` "Fix status" section, and its `GATE_NEGATIVE_TEST_EVIDENCE.md` row.
- Coexistence with devbreakfx14's line-31 fix `3a35f698f`. That commit is not on origin/dev yet and lives in `C:\wt\devbreakfx14_kkq37t`.

## Findings

### MED-1: library control flow and counter writes are not scanned; a `break` in a library silences the check itself

The per-library loop (lines 176-196) scans for the following:

- `exit`;
- `[Environment]::Exit` with any argument other than 124;
- dot-sources;
- the deny-list commands and members;
- duplicate function names.

It does not apply the following to the libraries:

- the LOW-3 break/continue rule (line 70);
- the return rule (line 72);
- the counter-assignment pins (`Get-Asg`, lines 88-110).

All of these still run only on `build_host_tests.ps1`'s AST.

- **N06**: add `if ($RunFailures -gt 0) { break }` at the top of `Get-HostTestVerdict` in `host_test_verdict.ps1`. Line 3620 of `build_host_tests.ps1` calls the function outside any loop, and PowerShell break is dynamically scoped, so the script ends with exit 0 whenever any test fails. This was confirmed empirically: a library function doing `break`, called from `try { F; exit 1 } finally {}`, exits 0. The check calls the same function at its own top level (line 15, `-RunFailures 1`), so **the check also stops after two `ok:` lines and exits 0 without printing `PASS`**. That took 2.2 s, against 13.6 s for the baseline. `run_all_checks.ps1` judges on exit code only (`Complete-CheckResult`), so it would count this as a PASS.
- **N07**: add `$script:failedExes = @()` at the top of `Invoke-HostTestProcess`. A function defined by a dot-sourced file writes the caller's script scope, which was confirmed empirically. Every run therefore wipes the earlier failures, and only the last executable's result survives.

Suggested fix:

1. Run the `Has-LoopBeforeFn` and return rules over every library AST.
2. Run the `Get-Asg` counter scan over every library AST, refusing any assignment in a library to `failedExes`, `builtExes`, `buildFailures`, `auxBuildFailures`, `totalExpected` or `onlySelected`.
3. Make the check itself break-proof. Put the body inside `foreach ($once in 1) { ...; $reached = $true }`, then follow it with `if (-not $reached) { FAIL }` before the final verdict. A stray dynamic `break` then lands on that loop, not on the script.

### MED-2: REVIEW_DEVBREAKFX13 MED-1 is only closed for the exact spelling

`$okSrc` (line 150) counts only assignments whose left side is literally `$exeExit`.
The `-ExePath` argument the runner receives is not pinned at all. Each of the
following hides every run failure:

| Mutant | Edit, in `Invoke-HostTestRun` or its caller | Why it passes |
|---|---|---|
| N01 | `[int]$exeExit = 0` before the `if` | left text is `[int]$exeExit` |
| N02 | `$local:exeExit = 0` | left text has a scope prefix; `Norm-Name` is not applied here |
| N03 | `${exeExit} = 0` | braced form |
| N04 | `$ExePath = "$env:SystemRoot\System32\hostname.exe"` before the call | the call text is unchanged, but the runner runs `hostname.exe` (exit 0) |
| N05 | `Complete-HostTestQueue`: `Invoke-HostTestRun -Name $it.Name -ExePath $env:ComSpec` | call sites of `Invoke-HostTestRun` are not pinned; `cmd.exe` on empty stdin exits 0 |

Each of N01-N03 was confirmed empirically to reassign the variable.

Suggested fix:

1. Find `$exeExit` writes by the normalised variable name (`Norm-Name` plus brace stripping), the way the counters already are, and include `ConvertExpressionAst` left sides.
2. Refuse any assignment to the parameters `$ExePath` and `$Name` inside `Invoke-HostTestRun`.
3. Pin both `Invoke-HostTestRun` call sites verbatim: `-Name $Name -ExePath $ExePath` at :590 and `-Name $it.Name -ExePath $it.Exe` at :627.

Even with these pins, `$it.Exe` and `$ExePath` are produced by over 90
`Invoke-HostTestExe` call sites. That data flow is beyond any static pin, so the
`kilnfw-host` negtest preset remains the backstop.

### LOW-1: the new deny-list is bypassed by computed names and by command shadowing

These remain MISSED. Each has the runtime effect noted, checked with a standalone script:

| Mutant | Construct | Effect |
|---|---|---|
| N08 | `& ('Invoke-' + 'Expression') 'exit 0'` | `GetCommandName()` is null for a computed command, so the name ban never sees it; the script exits 0 |
| N10 | `Remove-Item -Path ('Variable:' + 'failedExes')` | the provider-path regex needs the element text to start with `variable:`; the counter is removed, so `.Count` is 0 |
| N13 | `$s='SessionState'; $v='PSVariable'; $global:ExecutionContext.$s.$v.Set('failedExes', @())` | the UserPath `global:ExecutionContext` misses `^ExecutionContext$`, and computed member names miss the member ban |
| N09 | `function Start-Process { ... }` in `build_host_tests.ps1`, which runs `cmd /c exit 0` whenever `-RedirectStandardInput` is given | a script function shadows the cmdlet that `Invoke-HostTestProcess` resolves (confirmed: a library runner returned 0 instead of 3). The duplicate-name check compares only the five files with each other, never with cmdlets |

Suggested fix, to be accepted as incomplete by nature:

- Refuse `&` or `.` with a non-constant command element.
- Refuse any member access whose member is not a constant (`.$x`).
- Strip the scope prefix before matching `ExecutionContext`.
- Refuse provider cmdlets (`*-Item`, `ri`, `gi`, `si`, `ni`) whose path element is not a plain constant.
- Refuse any function in the five units whose name `Get-Command -CommandType Cmdlet,Application` resolves.

### LOW-2: a new source-path-drift false positive at line 171

`check_source_path_drift.ps1` on 713c474d8 reports two lines, `:31` (INFO-3, devbreakfx14's)
and a new one:

```
tools/check_host_test_verdict.ps1:171: Join-Path references '$ln.ps1' which does not exist ...
```

devbreakfx14's `3a35f698f` touches only line 31. It applies cleanly on top of
fx15, and `check_host_test_verdict.ps1` still passes with it applied (verified
with `git apply` and then `git apply -R`). However, the drift check **stays
FAILED on :171**, so landing devbreakfx14 alone will not turn it green.

Suggested fix: build the path without a literal `Join-Path`, for example
`[System.IO.Path]::Combine($PSScriptRoot, "$ln.ps1")`. Alternatively, fold this
line into devbreakfx14 before it lands.

### LOW-3: the behavioural test can be gamed, and INFO-5 is still open

- **N11**: in `host_test_exec.ps1`, `return $(if ($ExePath -like '*.bat') { $p.ExitCode } else { 0 })`. The test drives only `.bat` files (run by `cmd.exe`), while real host tests are `.exe` files. Suggested fix: add real-exe cases. `where.exe` and `findstr.exe` with no arguments and empty stdin exit 2, and `whoami.exe` exits 0 (measured). This does not stop a mutant that keys on the path, but it does close the file-type split.
- **N12**: `-or $RunFailures -gt 0 -or` becomes `-eq 1`. This is REVIEW_DEVBREAKFX13 INFO-5, still MISSED, and the fix status says "not done". One case with `-RunFailures 5` and one with `-BuildFailures 3` would close it.

### INFO-1: the `${function:Get-KilnBuildGateHoldDecision}` exception is acceptable but wider than needed

The `$ec` exemption (line 185) matches on exact text in every unit and in any
syntactic position, including the left side of an assignment.

- In `build_host_tests.ps1`, the separate `$fnDrive` scan (line 82) still refuses it. N14, `${function:Get-KilnBuildGateHoldDecision} = { 'keep' }`, was CAUGHT by `FAIL: [${function:...}] / alias: drive access is not used`.
- In a library, the same assignment would be allowed. It could change only the build-gate watchdog's hold decision, whose kill path is `[Environment]::Exit(124)`, which is nonzero. It is therefore not a verdict bypass.

Tightening suggested: limit the exemption to the `build_gate.ps1` unit and to a
read position, meaning its parent is a `CommandParameter`/argument, not an
assignment's left side.

### INFO-2: brittleness on future legitimate edits

`build_gate.ps1` and `build_lock.ps1` are shared libraries that many scripts use.
Any of the following in either file now fails `check_host_test_verdict` with a
LOW-1/LOW-2 message that does not name the library's own purpose:

- a second `${function:...}` handed to the watchdog runspace;
- a `Get-Variable`;
- a dot-source;
- an `[Environment]::Exit(1)`;
- a function name reused in `build_host_tests.ps1`.

The failure is loud and points at the file and line, so this is acceptable.
A one-line header comment in each library naming this check would save the next
editor a search.

The following are pre-existing exact-text pins and are whitespace-sensitive:

- the dot-source list (line 165);
- the runner call (line 151);
- the outer finally.

The rule "no catch anywhere in build_host_tests.ps1" will refuse a future
harmless `try/catch`. That is the intended trade.

### INFO-3: the real-process runs are hermetic and quick

- The check uses a per-run `htv_<guid>` directory under `%TEMP%`, and the runner uses `hostrun_<guid>.*` files. No shared or fixed path is used, so parallel runs and `run_all_checks` phase 2 are safe.
- The whole check takes 11.7-13.6 s, of which the timeout case is about 1 s plus `taskkill /T`.
- The `$global:HostTestTimedOut` side effect is reset afterwards.
- Two edge cases remain. First, if `taskkill` fails, the orphaned `ping -n 30` holds the directory, and `Remove-Item -ErrorAction SilentlyContinue` leaks it. Second, `.bat` files run through `cmd.exe` without `/d`, so an HKCU `AutoRun` entry would run as well. Neither affects the verdict.
- An exception thrown while the process runs propagates under `$ErrorActionPreference = "Stop"`, so it fails closed.

### INFO-4: closure of REVIEW_DEVBREAKFX13 findings

| Finding | Status |
|---|---|
| MED-1 | closed for B09/B10 (re-run: CAUGHT); spelling and argument variants open (MED-2) |
| LOW-1 | closed for B11-B13 and a library `exit` (re-run: CAUGHT); library control flow and counter writes open (MED-1) |
| LOW-2 | closed for the literal forms; computed forms open (LOW-1) |
| LOW-3 | closed in `build_host_tests.ps1` (B03 re-run: CAUGHT); not applied to the libraries (MED-1) |
| LOW-4 | closed |
| INFO-1 | done; the outer try is selected by `Parent -eq $ast.EndBlock` |
| INFO-2 | done |
| INFO-6 | done |
| INFO-5 | still open (N12) |
| INFO-3 | coexists with devbreakfx14 `3a35f698f`, but see LOW-2 |

## Negative test

Command, run from `C:\wt\rvdevbreakfx15_hx8tx8`:

```
tools\negtest.ps1 -Preset check -PresetArg tools/check_host_test_verdict.ps1 -Mutations <scratch>/rvfx15_mut.json -ExpectPattern "(?m)^\s+FAIL: "
```

The base was 713c474d8. The baseline PASSED in 13.6 s, with
`real_tree_unchanged: true` and `copies_removed: true`. The log directory is
`%TEMP%\negtest_logs\20261012_094246_9s68`. Every CAUGHT below has a named
`FAIL:` line.

| # | Mutant | Verdict | Matched FAIL / note |
|---|---|---|---|
| N01 | `[int]$exeExit = 0` before the guard | MISSED | MED-2 |
| N02 | `$local:exeExit = 0` | MISSED | MED-2 |
| N03 | `${exeExit} = 0` | MISSED | MED-2 |
| N04 | `$ExePath = ...\hostname.exe` before the runner call | MISSED | MED-2 |
| N05 | queue call site `-ExePath $env:ComSpec` | MISSED | MED-2 |
| N06 | `break` in `Get-HostTestVerdict` when `RunFailures > 0` | MISSED | MED-1; the check exits 0 after two `ok:` lines, with no PASS |
| N07 | `$script:failedExes = @()` in `Invoke-HostTestProcess` | MISSED | MED-1 |
| N08 | `& ('Invoke-' + 'Expression') 'exit 0'` | MISSED | LOW-1 |
| N09 | `function Start-Process` shadow in `build_host_tests.ps1` | MISSED | LOW-1 |
| N10 | `Remove-Item -Path ('Variable:' + 'failedExes')` | MISSED | LOW-1 |
| N11 | runner returns the real code only for `*.bat` | MISSED | LOW-3 |
| N12 | verdict `RunFailures -gt 0` becomes `-eq 1` | MISSED | LOW-3 (INFO-5 carry-over) |
| N13 | `$global:ExecutionContext.$s.$v.Set('failedExes', @())` | MISSED | LOW-1 |
| N14 | `${function:Get-KilnBuildGateHoldDecision} = {...}` in `build_host_tests.ps1` | CAUGHT | `FAIL: [${function:...}] / alias: drive access is not used` |
| C1 | B09: `-ne 0` becomes `-lt 0` | CAUGHT | `FAIL: failedExes append sits in a bare if ($exeExit -ne 0) with no else (MED-1)` |
| C2 | B10: `return $p.ExitCode` becomes `return 0` | CAUGHT | `FAIL: Invoke-HostTestProcess: exit 3 returns 3 (got 0)` |
| C3 | `build_lock.ps1`: `function Exit-BuildLock { exit 0 }` | CAUGHT | `FAIL: build_lock.ps1 : no exit statement (LOW-1)` |
| C4 | B03: `function Stop-Early { break }; Stop-Early` | CAUGHT | `FAIL: every break/continue has a loop or switch ancestor before any function boundary (LOW-3) (bad: 3620)` |

The runtime effect of N01-N03, N06, N07, N08, N09 and N10 was confirmed with
small standalone scripts. One candidate, `Add-Member -Force` of a `Count` note
property on the counter array, was tested and is **not** a bypass, because
`.Count` still read 2.
