# Review: toolfx3 fixes for REVIEW_SUBFX (fa2c7d9a7, 80381eb3c, ad517c152, 3c515349c)

Date 2026-10-10. Reviewer: Opus. Read-only review of origin/dev at 75f6c37e2, done in a
`-NoSubmodules` worktree. No code was changed.

Scope:
- `tools/check_submodule_pins_pushed.ps1` and `tools/test_check_submodule_pins_pushed.ps1`
- the pin-check wiring in `tools/land.ps1` and `tools/dev_promote.ps1`
- the new pin case in `tools/check_land.ps1`
- `tools/check_touch_cal_exit_target_caller.ps1`

## Test results at dev tip (75f6c37e2)

The fixer did not rerun the tests on the final rebased tree, so I ran them here.

| Run | Result |
|---|---|
| `test_check_submodule_pins_pushed.ps1` | all 15 cases pass, 29 s |
| `check_touch_cal_exit_target_caller.ps1` | pass, 619 files scanned, 14 s |
| `check_land.ps1` | all cases pass, 49 ok lines (including the new pin case), exit 0 |
| `check_submodule_pins_pushed.ps1 -Commit HEAD` against the real repo | PASS in 3.5 s. All three pins (lvgl, mykicadMcp, TFT35-SPI) are remote tips. |

## Answers to the brief

1. **Can an unpushed pin still land or promote?** Only in the latent cases T-1 and T-2 below.
   Both need an ssh origin, or a non-interactive credential failure on origin. Today origin and
   all three submodule URLs are https on github.com. The exit-code mapping is now correct:
   land.ps1:344-346 and dev_promote.ps1:154-155 refuse every code except 0 and 3. That covers exit
   2 (unreadable tree), a missing script (-196608) and a host crash. With the origin probe,
   exit 3 now means that origin did not answer either, so the push that follows would fail as
   well. Every git call is bounded (60 s, process-tree kill), so ls-remote cannot hang land.
   The worst-case stall is the subject of T-3.
2. **False refusals that would block every landing?** None found.
   - The normal case passes in 3.5 s.
   - `.gitmodules` and the gitlinks are read from the commit (`config --blob`, `ls-tree`), never
     from the working tree. An empty submodule directory in a `-NoSubmodules` worktree is
     therefore irrelevant.
   - The mykicadMcp pin is a tip today. If the remote moves ahead, the pin falls back to
     `fetch --depth 1 <sha>`, which GitHub serves for reachable commits.
   - A proxy that blocks github.com while origin works would refuse, and that is the intended
     fail-closed result.
3. **Consumers of `submodule_pins`.** The only consumer is check_land.ps1:260. dev_promote emits
   no such field. Nothing else in tools/ or tools/PcTools parses it. The JSON shape is unchanged
   apart from that field.
4. **Tests.** See the table above.
5. **Discrimination.**
   - The new pin-script cases discriminate: S-2a (differing working-tree `.gitmodules`), S-2b
     (unnamed gitlink), origin-up/submodule-down and both-down.
   - check_land's pin case asserts `submodule_pins=fail`. That value is set only at the pin
     step, so a refusal from an earlier step cannot satisfy it.
   - The gaps are listed in T-5.

## Findings

### T-1 LOW -- the default GIT_SSH_COMMAND overrides the user's core.sshCommand / GIT_SSH -- FIXED (toolfx5)
`tools/check_submodule_pins_pushed.ps1:24` sets `GIT_SSH_COMMAND=ssh -o BatchMode=yes ...`
whenever that variable is unset. Git gives GIT_SSH_COMMAND precedence over both `GIT_SSH`
(plink) and `core.sshCommand`. A common Windows setup has `core.sshCommand` pointing at
`C:/Windows/System32/OpenSSH/ssh.exe` so that the Windows ssh-agent is used. On such a setup,
every ssh call in the check runs Git's bundled ssh with no agent:
- An ssh submodule URL gives `Permission denied (publickey)`. That is a FAIL, a false
  refusal.
- An ssh origin makes the origin probe (line 115) fail. A submodule DNS failure or timeout then
  stays SKIP (exit 3), and land pushes using the user's real ssh config. That reopens the S-1a
  hole.

This is latent, because every URL is https today. The env change is confined to the child
powershell, so it does not leak into land's own `git push`.

Fix: set the default only when `GIT_SSH_COMMAND`, `GIT_SSH` and `git config core.sshCommand` are
all empty. Otherwise reuse the configured command with BatchMode appended, as in
`-c core.sshCommand="<existing> -o BatchMode=yes"`.

### T-2 LOW -- the origin probe treats any failure as "origin unreachable" -- FIXED (toolfx5)
At `check_submodule_pins_pushed.ps1:115-116`, any nonzero exit from `ls-remote origin HEAD` keeps
the run at SKIP. That includes an auth error under `GCM_INTERACTIVE=never` and
`GIT_TERMINAL_PROMPT=0`.

Scenario:
1. The submodule host times out.
2. Origin's credential helper needs an interactive refresh. That fails in the check, which runs
   with prompts disabled.
3. The result is SKIP, so land warns and continues.
4. land's own `git push` runs in the user's normal environment, refreshes the credential and
   succeeds.

Fix: classify the probe output with `$netPattern`. Only a network-pattern failure keeps SKIP.
Any other probe failure means origin is reachable, so it should FAIL, or exit 2 with
"cannot decide".

### T-3 LOW -- bounded but long stalls; the suite test hits GitHub -- FIXED (toolfx5)
- In land, the worst case per push attempt is N x (60 s ls-remote + 60 s fetch) + 60 s for the
  origin probe. That is about 7 min with three submodules. The pin step sits inside the
  non-fast-forward retry loop (land.ps1:342), so it repeats on every attempt.
- The test's `no-arg` case (`test_check_submodule_pins_pushed.ps1:92-96`) runs the check against
  the real repo, so it calls `ls-remote` on github.com, three times plus the origin probe. That
  test runs inside `run_all_checks.ps1` (lines 360-365), whose comment says it is offline. On a
  machine whose network silently drops packets, it stalls for about 4 min. It never fails
  falsely, because it accepts PASS, SKIP or FAIL.

Fix: in the no-arg case, set `KILNCTL_SUBPIN_TIMEOUT_SEC=5`, or run it with the working directory
at a scratch repo. Its only purpose is the parameter-binding smoke test. In land, skip the re-run
when the rebased tree's gitlinks did not change (compare `ls-tree` output).

### T-4 LOW -- a fetch timeout is reported as "not on remote" -- FIXED (toolfx5; message only, no automated test: needs a fetch that hangs after a good ls-remote)
At `check_submodule_pins_pushed.ps1:103-105`, a fetch-by-sha that times out after a good ls-remote
(`$f.TimedOut`) prints `FAIL: <path> pins <sha>, not on <url>` and the instruction to push the
submodule. That message is wrong, and it sends the operator to push something that is already
pushed. It is still fail-closed. For the large lvgl repo on a slow link this is the realistic
false-refusal path.

Fix: when `$f.TimedOut`, print "fetch timed out; cannot confirm", and keep the FAIL.

### T-5 LOW -- test gaps (by inspection) -- FIXED (toolfx5)
- No check_land case drives an exit other than 0 or 1 from the pin script. Reverting S-4a at
  land.ps1:345 (for example, letting exit 2 fall through to the warning) is therefore not caught.
  A `-PinCheckScript` seam with stubs that exit 2 and 3 would close this. The 3 case should
  assert `submodule_pins=skipped` and that the push happened.
- No check_land case asserts `submodule_pins=pass` on the happy path. A mapping bug that labels
  a pass as 'skipped' or 'fail' and still pushes would go unnoticed.
- `check_dev_promote.ps1` has no pin case. Removing the pin call from dev_promote.ps1:153-155,
  or reverting it to warn on every nonzero code, passes every test.

### T-6 INFO -- a stale `$LASTEXITCODE` if `git rev-parse HEAD` fails -- FIXED (toolfx5)
At land.ps1:343, `(git rev-parse HEAD).Trim()` on `$null` raises a statement-terminating error
before the child runs. `$LASTEXITCODE` then still holds the 0 from the post-rebase checks script,
so `subPins='pass'` is recorded and the push proceeds. This is practically unreachable right
after a successful rebase.

Fix: resolve the sha into a variable first, and `Finish 1` if it is empty.

### T-7 INFO -- relative `.gitmodules` URLs would be a false FAIL
A `url = ../x.git` is passed verbatim to `ls-remote`. Git then resolves it against the working
directory, not against origin's URL, and the check FAILs. There are none today. If one is ever
added, resolve it with `git submodule--helper resolve-relative-url`, or against `remote.origin.url`.

### T-8 INFO -- touch_cal caller check: multi-line calls fixed, other residuals remain
The whole-text regex now catches a call split as name, newline, `(`. I confirmed by reading the
code that the 40-character look-behind classifies the declaration and definition correctly, and
that it does not treat `const char *p = fn(` as a definition, because that text ends with `= `.

Still uncounted:
- a function-pointer use (`= lcd_touch_cal_saved_exit_target;`), because the regex requires `(`
- a second `const char *lcd_touch_cal_saved_exit_target(` prototype or definition in another file,
  because `$defs -lt 2` is only a minimum

Acceptable as a guard against a careless second caller, as A1-a already concluded.

## Verdict

The SUBFX findings are fixed as claimed:
- S-2a, S-2b and S-1a's origin probe
- S-4a's exit-code mapping in both scripts
- the S-3/S-4 pin case
- A1-a's multi-line match

All tests pass at dev tip, and no false refusal was found on the normal path. T-1 and T-2 are
the only remaining ways for an unpushed pin to land, and both are latent while every URL is
https.
