# Review: fixes for REVIEW_SUBCHK_A1B1 (fd1905824, 295c8cae3)

Date 2026-10-10. Reviewer: Opus. This is a read-only review of origin/dev at 295c8cae3. No code was changed.

Scope:
- `tools/check_submodule_pins_pushed.ps1` (S-1, S-2) and `tools/test_check_submodule_pins_pushed.ps1`
- `tools/land.ps1` (S-3, S-4)
- `tools/PcTools/src/kilnctrl/mcp_server_debug.py` (B1-1) and its test
- the new `tools/check_touch_cal_exit_target_caller.ps1` (A1-1)

Verification run in a clean worktree at 295c8cae3:
- `test_check_submodule_pins_pushed.ps1`: all 10 cases pass in 46 s.
- `tests/test_pctools_batch_d_2026_10_10.py`: 31 passed.
- `check_touch_cal_exit_target_caller.ps1`: passes, 617 files scanned.
- negtest results are in the next section.

## Negative tests (tools\negtest.ps1 -RequireAssertion)

| Mutation | Test run | Result |
|---|---|---|
| S-2: `config --blob "${Commit}:.gitmodules"` -> `config --file .gitmodules` | test_check_submodule_pins_pushed.ps1 | **MISSED** |
| S-2: gitlinks-without-.gitmodules guard disabled (`if ($false)`) | same | CAUGHT (`gitlink-no-gitmodules`) |
| S-3: drop `-RepoPath $top -Commit ...` from land.ps1 | check_land.ps1 | CAUGHT, but only by accident (see S-6) |
| S-4: drop `submodule_pins` from the final JSON | check_land.ps1 | **MISSED** |
| S-4: `$script:subPins = 'pass'` unconditionally | check_land.ps1 | **MISSED** |
| land: remove the `exit 1 -> Finish 1` block | check_land.ps1 | **MISSED** |
| A1: second caller in kiln_ui.c | check_touch_cal_exit_target_caller.ps1 | CAUGHT |
| A1: second caller with `(` on the next line | same | MISSED (see A1-a) |

About the author's S-2 run ending in exit 124: I could not reproduce it. My S-2 run finished in 91 s:
a 48 s baseline, then about 40 s per mutation. negtest itself never exits 124; its timeout path exits 2.
Exit 124 comes from GNU `timeout` (the Bash wrapper) or from `tools\wait_for.ps1`, so the likeliest cause is
a wrapper bound shorter than baseline plus mutations. The check_land.ps1 negtest takes far longer:
a 239 s baseline and 130 to 230 s per mutation, about 15 min in total.

## Findings

### S-2a MED -- the S-2 fix has no effective test (negtest MISSED)
Both new S-2 cases empty the working-tree `.gitmodules` or delete it. In both, the gitlinks guard
(`gitlinks.Count -gt 0` with no cfg -> FAIL) fires whichever `.gitmodules` is read. Reverting to the
working-tree read therefore still passes every test. The review's actual S-2 scenario is a working tree
whose `.gitmodules` is present but differs from the commit's, for example missing the new entry or
naming a different path. That scenario is untested. Suggested test: the commit's `.gitmodules` names `m`
with an unpushed pin, and the working tree's names only `other`. The working-tree read would then print
`NOTE: ... nothing to check` and PASS.

### S-2b LOW -- a gitlink that `.gitmodules` does not name is never checked
`$gitlinks` is used only when `.gitmodules` yields no entries at all. If `.gitmodules` has entries, the loop
walks only those entries. A gitlink with no entry, or one whose `.gitmodules` path differs (for example a
moved submodule), is silently skipped, and the check can PASS. Suggested fix: every 160000 entry in
`ls-tree -r $Commit` must match a `.gitmodules` path, else FAIL.

### S-1a MED (residual) -- a DNS or timeout failure on the submodule host SKIPs, and land then pushes
The classification itself is sound. I probed it live:
- A missing GitHub repo prints `Repository not found` (exit 128) -> FAIL.
- With prompts disabled, a private or unauthenticated repo prints `could not read Username ... terminal
  prompts disabled` -> FAIL.
- `netPattern` matches only genuine transport errors.

But SKIP stays warn-only in land.ps1 and dev_promote.ps1. The original S-1 suggestion was "probe origin;
if origin answers and the submodule URL does not, FAIL", and it was not implemented. A real unpushed pin
can therefore get through in three ways:
- (a) A typo in the `.gitmodules` host that is not registered. It gives `Could not resolve host` -> SKIP.
  The new `dns-outage` case (`nonexistent-host.invalid`) codifies this as the expected result. A
  mistyped host that is registered but parked (tried: `githb.com`) gives a redirect error -> FAIL.
- (b) A transient network failure or a 60 s stall on the submodule host while the push to origin
  succeeds a few seconds later.
- (c) A credential helper that ignores `GCM_INTERACTIVE=never`, or an ssh URL waiting on a host-key
  prompt. `GIT_TERMINAL_PROMPT` does not cover ssh. The run waits out the timeout and then SKIPs.

The successful `git push origin` right after a SKIP contradicts "network unreachable". land could use
it: on SKIP, run a bounded `git ls-remote origin`, and if origin answers, FAIL. Or at least refuse when
the submodule host equals origin's host. All three current URLs are https on github.com, the same host
as origin.

### S-6 LOW -- the script's default `-RepoPath` is broken under Windows PowerShell 5.1; a crash reads as "pin not pushed"
`[string]$RepoPath = (Split-Path -Parent $PSScriptRoot)` fails at parameter binding. `$PSScriptRoot` is
empty in a param default under `powershell -File` on 5.1, which I reproduced. The script exits 1, which
land.ps1 and dev_promote.ps1 report as "a submodule pin is not on its remote". So:
- Running the script standalone, as its header documents, always fails.
- From ccb7b5d26 until fd1905824, land.ps1 called the script with no arguments, so every land.ps1 run
  was refused at that step with a misleading message.
- The S-3 negtest above was CAUGHT only because of this crash. If the default worked, the mutation
  would go uncaught, because no case asserts which repository or commit is inspected.

The failure direction is safe (it blocks). Fix: compute the default in the body, as in
`if (-not $RepoPath) { $RepoPath = Split-Path -Parent $PSScriptRoot }`, the way dev_promote.ps1 already
does. Also give a script error a distinct exit code (for example 2), and have land report it as an error
rather than as an unpushed pin.

### S-3/S-4 LOW -- the land.ps1 wiring is correct but untested
Reviewed by hand:
- land.ps1 does `Set-Location $top` at line 173, so `(git rev-parse HEAD)` is the rebased HEAD of the
  tree being landed, and `-RepoPath $top` is that tree.
- Exit 1 -> `Finish 1` before `git push`. The pin-check FAIL blocks.
- `submodule_pins` is pass, fail, skipped or not-run in the final JSON.

But check_land.ps1 has no pin case, so removing the block, mislabelling the field or dropping it all pass
(table above). Suggested test: a `-PinCheckScript`-style seam, or a scratch origin with a gitlink to an
unpushed sha. The test should assert refusal, `submodule_pins:"fail"`, and that origin/dev is unchanged.

### S-4a LOW -- exit codes other than 0, 1 and 3 count as "skipped" (warn) and land pushes
land.ps1 and dev_promote.ps1 treat anything other than 0 or 1 as "could not run". A missing script
(`powershell -File` exits -196608), an exit 2, or a host-level crash all become a yellow warning followed
by the push. Only exit 3 should be warn-only. Any other nonzero code should refuse.

### S-1b INFO -- the timeout kill is correct on Windows
`Start-Process` resolves `git` to `cmd\git.exe`, which spawns `mingw64\git.exe`, which spawns
`git-remote-http`. The script reads `$null = $p.Handle` so that `ExitCode` is populated (the PS 5.1
workaround). Output goes to temp files, so there is no pipe deadlock. `taskkill /T /F` kills the whole
tree. After the `timeout` case, no `git` or `git-remote-http` process for 127.0.0.1 or `ls-remote` was
left running.

Minor points:
- After a kill there is no wait before `finally` deletes the temp files. The delete can fail silently
  while handles are still closing, which leaks two temp files. Harmless.
- On success, stderr is concatenated into `Out` and split into "refs". Stray tokens cannot equal a
  40-hex sha. Harmless.
- A fetch timeout after a good ls-remote is a FAIL: a false refusal, never a false pass.

### B1-1 -- fixed correctly
The autotune branch is reached only after `get_exec_status()` answered with an idle state. When the exec
read fails, the code still takes the warning path and returns before the autotune read, so owner decision
B1 is kept. An autotune read failure now always refuses. The new test covers exec idle plus autotune
TimeoutError -> refused and `program` not called. The full file passes (31 tests).

### A1-a INFO -- the caller check is line-based
A call split across lines (`lcd_touch_cal_saved_exit_target\n(`) is MISSED, as the negtest above shows.
So is a function-pointer use (`= lcd_touch_cal_saved_exit_target;`) and a macro wrapper. This is
acceptable as a guard against a careless second caller, not against a deliberate one. A plain
identifier-occurrence count, with only the declaration and definition allowed outside
ui_page_touch_cal.c, would close all three.

## Answers to the brief

- **Can a real failure be classified SKIP and let an unpushed pin through land?** Yes, in the residual
  cases in S-1a: an unregistered host typo, a transient DNS failure or stall on the submodule host, and
  a prompt that ignores the env vars. Exit codes other than 0, 1 and 3 also count as skipped (S-4a). A
  missing repo, an auth failure or a bad scheme are FAIL.
- **Is the timeout kill correct on Windows?** Yes (S-1b).
- **Does land block when the pin check FAILs?** Yes. Exit 1 -> `Finish 1` before the push, with
  `submodule_pins:"fail"`; I saw this in the S-3 mutation's output. It is untested (S-3/S-4) and it also
  fires on a script crash (S-6).
