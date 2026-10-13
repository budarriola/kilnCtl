# Review: toolfx11 fix commits (8603fdaf8, c61e7af86, 61a733735) -- 2026-10-12

This is an adversarial, read-only review of the three origin/dev commits that fix the findings in
`REVIEW_TOOLFX11_2026-10-11.md`. It was done at `61a733735`. No reviewed code changed. On origin/dev
`053f60dee`, `tools/push_verify.ps1`, `tools/land.ps1`, `tools/check_push_verify.ps1`,
`tools/check_land.ps1` and `tools/lib_push_verify_walk.ps1` are still byte-identical to that base.

All mutation evidence comes from `tools\negtest.ps1 -Preset check` with `-Mutations` JSON files.
The unmutated baseline passed in both runs, and both runs ended with `real_tree_unchanged: true`:

- `-PresetArg tools\check_push_verify.ps1`: baseline 63 s, 11 mutations.
- `-PresetArg tools\check_land.ps1`: baseline 373 s, 3 mutations.

Live demonstrations ran against throwaway repositories in a scratch directory, never against the
real origin.

## Verdict

- **push_verify cannot report LANDED falsely on any path I found.**
  - LANDED needs `$fetchOk`.
  - `$fetchOk` needs `WaitExit` on the held native handle to succeed and `ExitCode` to read 0.
    `ExitCode` returns -1 when `GetExitCodeProcess` fails.
  - A `Launch` exception ends the try block. The finally runs, and the script reaches
    `if (-not $fetchOk)`, which prints UNKNOWN and exits 1.
- **The handle discipline is correct.**
  - The thread handle is always closed.
  - `Launch` closes the process handle only when it does not hand it back (`keep` is false).
  - The caller closes it exactly once, in its finally, before the job.
  - I found no double close and no use after close.
- **The kill scope is correct.**
  - The job holds only the suspended-then-resumed git tree.
  - The snapshot walk keeps only descendants created after their own parent and re-checks each
    StartTime through an open handle.
- **land.ps1's ref-lock race detection does not work against any real git race (MED-1).** It
  fails closed, so a race still never lands silently. But the retry this fix chain exists for never
  fires, and the check that is meant to prove it passes only because it fakes a message that real
  git does not produce in that form.

## Findings

### MED-1: the race regex matches no real race output (`tools/land.ps1:375`, `:381`)

`land.ps1:381` counts a push rejection as a race only when the output has a line matching
`error: cannot lock ref '<ref>': is at <X> but expected <Y>` at end of line. No real race output
meets that:

1. **Current git (2.50 and later, 2.52 used here).** Ref updates are batched in receive-pack, and a
   lost race reports only `! [remote rejected] HEAD -> dev (incorrect old value provided)`. There is
   no `cannot lock ref` line at all.
   - Reproduced for real against a scratch bare origin. A pre-receive hook ran
     `git update-ref refs/heads/dev <other>` once, with quarantine env vars unset, so the ref really
     moved under the push.
   - land.ps1, run from a copy of this tree, printed `REFUSED/FAILED: git push was refused by the
     remote (hook / protected branch), not a race` and exited 1 with no retry.
2. **GitHub.** GitHub reports the race as the status reason:
   `! [remote rejected] dev -> dev (cannot lock ref 'refs/heads/dev': is at X but expected Y)`.
   - There is no `error:` prefix, so the regex does not match.
   - The pre-fix unanchored regex did match this form. That makes this a regression against the
     real origin.
   - Mutation `probe_race_github_remote_rejected_form` changed check_land's race hook to print
     this form. It was CAUGHT, meaning land did not retry:
     `FAIL: landed after cannot-lock-ref retry`, `FAIL: rebased twice after ref-lock race (got 1)`.
3. **Older git's own stderr line, with real 40-hex object names.**
   - `land.ps1:375` captures output through `& git push ... 2>&1 | Out-String` under Windows
     PowerShell 5.1.
   - The first stderr line becomes an ErrorRecord, and Out-String wraps it at the host width
     (about 120 columns). That puts a line break between `but expected` and the sha, so `\S+\s*$`
     on that line never matches.
   - A child `powershell -File` run with full shas gave `match=False` for `refs/heads/dev` and for
     a longer ref name.
   - Mutation `probe_race_full_40hex_shas` changed only `aaaa`/`bbbb` in check_land's race hook to
     40-hex values. It was CAUGHT with the same two FAIL lines.

**Test gap.** The race cases in `tools/check_land.ps1:230-248` are vacuous against reality. They
fake a 4-character-sha `error: cannot lock ref` line that stock git emits only with 40-hex names,
and current git does not emit it at all.

**Impact.** The race is never retried. Every real ref race on dev ends in a hard refusal whose
message says "hook / protected branch, not a race", which sends the operator the wrong way. No
false LANDED results.

**Fix direction.**
- Run the push with `--porcelain` and `LC_ALL=C`, and capture without ErrorRecord wrapping (for
  example `cmd /c "git push ... 2>&1"`, or redirect to a file).
- Classify the bracketed reason of the pushed ref's own status line, strictly, as either
  `incorrect old value provided` or `cannot lock ref '<ref>': is at <40hex> but expected <40hex>`.
- Make check_land produce a real race: a pre-receive hook that unsets `GIT_QUARANTINE_PATH`,
  `GIT_OBJECT_DIRECTORY` and `GIT_ALTERNATE_OBJECT_DIRECTORIES` and moves the ref once with
  `git update-ref`.
- Keep one case with the GitHub status-reason form, which a hook can echo.

### LOW-1: "anchored" race regex still matches mid-line hook prose (`tools/land.ps1:381`)

- `(?:^|\s)` lets the pattern start after any whitespace.
- A hook line such as `remote: policy says error: cannot lock ref 'x': is at x but expected y`
  matched (`match=True`), so that hook refusal is retried as a race.
- The retry count bounds the damage, which is why this is LOW.
- The MED-1 fix should anchor on git's own status line rather than on free text.

### LOW-2: `KillProc` as a no-op is not an equivalent mutant (`tools/push_verify.ps1:230`)

`toolfx11`'s fix status called this mutant equivalent. The test misses it, but it is not
equivalent:

- On the no-job path, the snapshot walk excludes the root (`Get-PvDescendantOrder`). `KillProc` is
  the only thing that kills the root git.
- check_push_verify always launches `C:\Program Files\Git\cmd\git.exe`, which is a launcher, and
  uses an http remote. Both guarantee at least one descendant, and the root exits once that
  descendant dies. So the mutant passes the check: MISSED (`killproc_noop`).

**Live demonstration (scratch copy of push_verify.ps1).**
- Setup:
  - `Get-Command git` resolved to `mingw64\bin\git.exe`, which is what a Git Bash PATH gives.
  - The remote was a `git://127.0.0.1:<port>` listener that never answers. git:// connects in
    process, so there are no descendants.
  - `PUSH_VERIFY_TEST_NO_JOB=1`.
- Unmutated: `walk found 0 descendant(s)`, UNKNOWN, `SURVIVORS=0`.
- With `KillProc` as a no-op:
  - push_verify exited, but the fetch git (`git -C ...\repo fetch --no-tags origin ...`) stayed
    alive indefinitely.
  - Because `bInheritHandles` is TRUE (`tools/push_verify.ps1:162`, INFO-2 of the previous
    review), the survivor also held the caller's inherited stdout pipe and sockets. The caller's
    `| Out-String` never returned, and the run had to be stopped.

`land.ps1:397` runs push_verify on the console of the caller. A leaked git there would hang the
agent tool call that ran land.

**Fix direction.** Add a no-job case whose root has no descendants (a git:// listener, with git
resolved to `mingw64\bin\git.exe`) and assert that the root is gone.

### LOW-3: L1 kill lint is a denylist with seven demonstrated holes (`tools/check_push_verify.ps1:85-94`)

Each mutation below inserts dead code (`if ($false) { ... }`) that kills by PID. The lint should
reject every one of them.

| Mutation | What it inserts | Verdict |
|---|---|---|
| `lint_control_stop_process` (control) | `Stop-Process -Id $fetchPid` | CAUGHT (`FAIL: S2/L1 ...`) |
| `lint_cmd_taskkill` | `cmd /c taskkill /F /PID $fetchPid` | MISSED |
| `lint_start_process_taskkill` | `Start-Process taskkill -ArgumentList "/F /PID $fetchPid"` | MISSED |
| `lint_cim_terminate` | `Invoke-CimMethod -Query "... ProcessId=$fetchPid" -MethodName Terminate` | MISSED |
| `lint_pvjob_killproc_getprocess` | `[PvJob]::KillProc((Get-Process -Id $fetchPid).Handle)` | MISSED |
| `lint_dynamic_stop_process` | `& ('Stop-' + 'Process') -Id $fetchPid` | MISSED |
| `lint_csharp_openprocess_h` | C# `OpenProcess(1,false,pid)` then `TerminateProcess(h, 1)` | MISSED |
| `lint_lib_walk_stop_process` | `Stop-Process -Id 1` in `tools/lib_push_verify_walk.ps1` | MISSED |

Why they get through:
- The `[PvJob]` member exemption trusts any argument.
- The C# rule allows `TerminateProcess` on any variable named `h`.
- The library file is not linted at all.

**Fix direction.** Use an allowlist instead:
- Forbid `Get-Process`, `Start-Process`, `Invoke-CimMethod`, `cmd` and non-literal command names
  in push_verify.ps1 and the library.
- Pin `TerminateProcess` to its two known call sites.
- Pin `KillProc` to a single call whose argument is `$fetchH`.

### LOW-4: `PUSH_VERIFY_TEST_NO_JOB` is not gated by `PUSH_VERIFY_TEST_MODE` (`tools/push_verify.ps1:200`)

The other two test hooks (lines 196 and 205) require `PUSH_VERIFY_TEST_MODE=1`. This one does not.
A stray `PUSH_VERIFY_TEST_NO_JOB=1` in a real environment silently downgrades every production
fetch to the snapshot-walk fallback. That is the weaker path, and LOW-2 shows it leaking a root
git. Gate it the same way, and extend the existing stray-hook case to cover it.

### INFO-1: hook removal outside try/finally (`tools/check_land.ps1:225`), mutation run as asked

`hook_removal_outside_finally_F11` replaced the F11 case's
`try { Run-Land } finally { Remove-Item $hook }` with sequential statements. Result: MISSED, exit 0.

This is an equivalent mutant in practice. `Run-Land` spawns a child powershell and does not throw
on a land failure, so the removal always runs. The try/finally only matters if the harness itself
throws, for example on Ctrl+C. Without it, a stale hook would break every later case, and that
would fail the check loudly. Keep it. No test change is needed.

### INFO-2: the post-launch-sleep seam is equivalent for production but load-bearing for the MED-1 test

- Production never sets `PUSH_VERIFY_TEST_MODE`, so removing the seam (`tools/push_verify.ps1:205`)
  changes nothing there. The fixer's equivalence claim holds for production.
- The seam is what makes the MED-1 regression test real. The mutation that reintroduces the
  original bug closes the native handle after launch and re-opens git by PID with
  `Get-Process -Id $fetchPid`:
  - With the seam: CAUGHT (`FAIL: git exits 0 long before the exit code is read -> still LANDED`).
  - With the seam also removed: MISSED, because git is usually still running at the re-open.

Keep the seam. A note next to it saying it is the MED-1 test's only lever would stop a later
cleanup from removing it as dead code.

### INFO-3: localization

- land.ps1 sets no `LC_ALL`/`LANG`. `error:` and `cannot lock ref` are translatable git messages;
  the bracketed `[remote rejected]` token is not. On a localized git the refusal branch at
  `:382` still fires, so a race is again reported as a hook refusal.
- push_verify does not parse git text (exit code only), so it is unaffected.

### INFO-4: `-FetchTimeoutSec` above about 2147483 overflows the `int ms` argument (`tools/push_verify.ps1:206`)

Found by reading the code, not run. `$FetchTimeoutSec * 1000` becomes a double that does not fit
`int`, so the call throws inside the try and the run ends UNKNOWN, which fails closed. git is
killed by the job close only when it was assigned to the job. A `ValidateRange` on the parameter
would remove the edge.

### INFO-5: `Get-Command git -CommandType Application` can return a `.cmd`/`.bat` shim

If the first `git` on PATH is a batch shim, `CreateProcessW` with it as `lpApplicationName` fails,
`Launch` throws, and the result is UNKNOWN. It fails closed, but with an unhelpful message.

## Questions from the dispatch, answered

| Question | Answer |
|---|---|
| False LANDED? | None found. |
| Wrong race classification? | Yes, MED-1 and LOW-1. Every real race is called a refusal, and prose that mentions a race can be retried. |
| Handle leak, double close or use after close? | None, including the exception paths. |
| Can the job or the walk kill outside the git tree? | No. |
| Can the test hooks trigger in real use? | Yes for `PUSH_VERIFY_TEST_NO_JOB` (LOW-4). The other two need `PUSH_VERIFY_TEST_MODE=1` and are capped. |
| Do the regex and git's real stderr agree across versions and locales? | No (MED-1, INFO-3). |
| Was `KillProc` as a no-op equivalent? | No (LOW-2). |
| Was removing the seam equivalent? | For production yes; for the MED-1 test no (INFO-2). |
| Hook removal out of try/finally? | Run: MISSED, equivalent in practice (INFO-1). |

## Mutation evidence summary

**`check_push_verify.ps1`:**
- CAUGHT: `bypid_reopen_with_seam`, `lint_control_stop_process`.
- MISSED: `killproc_noop`, `bypid_reopen_seam_removed`, `lint_cmd_taskkill`,
  `lint_start_process_taskkill`, `lint_cim_terminate`, `lint_pvjob_killproc_getprocess`,
  `lint_dynamic_stop_process`, `lint_csharp_openprocess_h`, `lint_lib_walk_stop_process`.

**`check_land.ps1`:**
- MISSED: `hook_removal_outside_finally_F11` (equivalent in practice).
- CAUGHT: `probe_race_full_40hex_shas` and `probe_race_github_remote_rejected_form`. These two are
  probes, not mutants of the code under review. CAUGHT here means land.ps1 does not retry those
  realistic race forms.

## Fix status

| Finding | Status | Fix / evidence |
|---|---|---|
| MED-1 | fixed | `land.ps1` uses `git push --porcelain` under `LC_ALL=C`/`LANG=C`; `Get-PushRejectVerdict` classifies only `!<TAB>` status lines from the bracketed reason (`incorrect old value provided`, `cannot lock ref ... is at <40hex> but expected <40hex>`, fetch first, non-fast-forward = race, retried). `check_land.ps1` now has a real race against a scratch bare origin (pre-receive hook advancing the ref) plus 11 verdict unit cases with real shas and the GitHub form. |
| LOW-1 | fixed | Hook / protected-branch / stale-lock declines and `remote:` prose classify as refused or other, never retried. Short-sha and unanchored-status-line cases in the verdict table. |
| LOW-2 | fixed | New no-job case in `check_push_verify.ps1`: mingw64 `git.exe` (no descendant) against a hanging `git://` remote; the root process must be killed. |
| LOW-3 | fixed | Kill lint is a PowerShell-AST allowlist over `push_verify.ps1` and `lib_push_verify_walk.ps1`, with pinned C# `TerminateProcess`/`TerminateJobObject`/`KillProc` sites; in-test hole cases for every demonstrated hole. |
| LOW-4 | fixed | `PUSH_VERIFY_TEST_NO_JOB` is honoured only with `PUSH_VERIFY_TEST_MODE=1`; stray-NO_JOB test added. |
| INFO-1 | equivalent, left | Hook removal outside `finally` has no observable effect in the check. |
| INFO-2 | fixed (comment) | Post-launch-sleep seam documented as test-load-bearing. |
| INFO-3 | fixed | `LC_ALL`/`LANG` set around the push. Mutant `land_no_lc_all` MISSED: equivalent, the test host locale is already English. |
| INFO-4 | fixed | `-FetchTimeoutSec` above 2000000 exits 2; test added. |
| INFO-5 | open | `.cmd`/`.bat` git shim resolution not addressed. |

Mutation evidence (`tools/negtest.ps1 -Preset check`):
- `check_land.ps1`: 8 mutants, 7 CAUGHT, 1 MISSED (`land_no_lc_all`, equivalent).
- `check_push_verify.ps1`: 6 lint-hole mutants CAUGHT; `pv_killproc_noop` CAUGHT (pinned `KillProc` body in the lint); dead-call `pv_killproc_call_dead_lint_clean` CAUGHT by the runtime root-only test ("root git itself is killed").
