# Review: docfx5 (919121e24, 7bb65da8a), 2026-10-12

These two commits on origin/dev are docfx5's fix for REVIEW_DOCFX4 LOW-1. Each git or holder wait now has a bound, and every timeout path kills the whole process tree with `taskkill /T /F` instead of `Process.Kill()`. Four files changed:

- `tools/check_build_gate_usage.ps1` (the LOW-1 site itself)
- `tools/check_build_lock_holder.ps1` (`Start-Holder` READY wait)
- `tools/check_doc_hash_citations.ps1` (`Invoke-BatchCheck`)
- `tools/wt_status.ps1` (the C# `WtStatusWalk.Git` helper)

The fixer's negtest covered only check_build_gate_usage. This review ran hang negtests on the other three at `7bb65da8a`. Every baseline passed. Every run reported the real tree unchanged, and every copy was removed.

**Safety verdict for wt_status:** a slow or hung git now fails closed. The directory is classed UNKNOWN ("git query failed"), and `-Prune` skips it. A 3 s bound made every git call time out, and the fixture's `stale_clean` worktree still came out UNKNOWN. Making the timeout path report exit 0 did not change that.

Counts: HIGH 0, MED 0, LOW 2, INFO 8.

## LOW

- **LOW-1** (`tools/check_doc_hash_citations.ps1:335`): the stdin write is not bounded. It runs before the 120 s bound at line 337.
  - The batch is one `StandardInput.Write` of about 6040 lines, far more than the roughly 4 KB pipe buffer. If git hangs before it reads stdin, the write blocks for as long as the hang lasts. The `WaitForExit(120000)` at line 337 is never reached.
  - Negtest D1 confirms this. Git was replaced by a hang that never reads stdin (`!sleep 177`). The check blocked for the whole hang, then failed at line 341 with "returned 0 lines for 6037 inputs". It never printed the 120 s message. The run scored MISSED at 188.7 s.
  - D2 hangs only after it has read all of stdin (`!cat >/dev/null; sleep 171`). It printed "did not exit within 120 s" at 129.6 s, with no surviving `sleep`. The bound works once the write has finished.
  - The check still ends, because the hang ends. A git that genuinely deadlocks without reading stdin would hang the check forever.
  - Fix: run the write on a task and bound it with the same deadline. For example, `$wt = $p.StandardInput.WriteAsync($payload)`, then `if (-not $wt.Wait(120000)) { taskkill ...; throw ... }`, then `Close()`. The alternative is to write from a separate runspace or thread.
- **LOW-2** (`tools/wt_status.ps1:106`): `err.Wait()` has no bound.
  - It runs only after `outT` has completed and `p` has exited. In practice stderr is already at EOF by then. The exception is a grandchild that inherited the stderr handle and is still alive; then this wait lasts as long as that grandchild does.
  - That is the same failure shape this fix closes for stdout. It is also the one wait in the helper still left without a bound.
  - Fix: `if (!err.Wait(15000)) { /* tree-kill as above */ }`, or simply `err.Wait(15000);`, since stderr is discarded anyway.

## INFO

- **INFO-1** (`tools/wt_status.ps1:96-103`, `$gather`/`Get-Class`): the fail-closed analysis that negtests W1 and W2 confirm.
  - A timeout returns `ExitCode -1` with no lines. A failure of `log`, `status --ignored`, `rev-parse --absolute-git-dir` or `status v2` sets `GitOk=false`. `Get-Class` returns UNKNOWN "git query failed", and it checks that before STALE_CLEAN.
  - A `cherry` failure marks every ahead commit unlanded, which gives HAS_WORK.
  - Prune re-runs `$gather` per directory (line 405-407), so a git that is slow only during the prune pass also leaves the directory alone.
  - W2 changed the timeout path to `ExitCode = 0` with empty output, and the result was still UNKNOWN. An empty `rev-parse` sets `GitOk=false` independently, so that is a second layer of defence.
- **INFO-2** (`tools/wt_status.ps1:160, 174, 371, 413, 428`): five native calls in wt_status still have no bound:
  - line 160: `git rev-parse --show-toplevel`
  - line 174: `Get-Registered`'s `git worktree list --porcelain`
  - line 371: `git rev-parse --short`
  - line 413: `worktree_mint.ps1 -Remove`
  - line 428: `git worktree prune`

  A hang in any of them hangs the tool, but none can cause a wrong prune. If `Get-Registered` fails, it returns an empty map. A registered worktree then looks unregistered, and its `.git` file makes it UNKNOWN "unregistered but has a .git". Bound them only if a hang is ever seen.
- **INFO-3** (`tools/wt_status.ps1:109`): the `catch (Exception)` path sets -1 but does not kill.
  - A throw after `Process.Start` (for example an AggregateException from a faulted read task) leaves git running. Not observed. Fails closed like the timeout path.
- **INFO-4** (all four files): `taskkill /T` finds children by ParentProcessId alone, with no creation-time check.
  - The root PID cannot be reused while the kill runs, because the open .NET Process object keeps its handle. A child, however, could have been spawned by an earlier process that held a now-reused parent PID, and taskkill would kill it too.
  - The window is tiny, and the tree here is short-lived (launcher -> mingw git -> sh). `negtest.ps1`'s `Add-Descendants`/`Test-ChildAdoptable` already filter by `CreationDate`; reuse that if this ever matters. Same as `run_all_checks.ps1:886`.
- **INFO-5** (`tools/wt_status.ps1:98-100`): taskkill's stdout is not redirected, so a timeout prints "SUCCESS: The process with PID ..." even under `-Quiet`.
  - Separately, `"taskkill"` and `"git"` are resolved through the CreateProcess search order, which includes the current directory. wt_status runs from the repo root, so a stray `git.exe` or `taskkill.exe` there would be picked up.
  - Fix (optional): set `RedirectStandardOutput`/`RedirectStandardError` on `tk`, and use `%SystemRoot%\System32\taskkill.exe`.
- **INFO-6** (`tools/check_build_lock_holder.ps1:44-47`): the READY wait is correct, with three points:
  - **No ReadLineAsync race.** A second read never starts while one is pending, because a timeout throws.
  - **Failure output.** A timeout ends the script through `throw` (exit 1, an error record) rather than a `FAILED:` line. That is the script's existing error style.
  - **Pre-existing behaviour, unchanged:**
    - EOF before READY (`$null`) still breaks out and returns the holder as if it were ready.
    - Holders from earlier scenarios are not killed in the top-level `finally`.
- **INFO-7** (negtest B2, W, D2): the tree-kill itself cannot be observed by any of these checks.
  - B2 removes the taskkill and is still CAUGHT with 0 survivors. The holder is a direct child that is killed when the throwing script and negtest's cleanup tear down.
  - W and D2 are CAUGHT on the timeout message with 0 `sleep` survivors. That shows the tree is gone after the run, not which kill removed it.
  - DOCFX4's hand measurement remains the evidence that `Process.Kill()` alone leaves `mingw64 git`, `sh` and `sleep` running.
- **INFO-8**: worst-case run time.
  - wt_status makes up to 5 git calls per registered directory, each bounded at about 255 s (120 + 120 + 15). With all 17 fixture worktrees timing out at a 3 s bound, W1 took 612 s.
  - With real 120 s bounds, a wholly hung git on a large `C:\wt` would take hours before it reported. It would still be correct and fail closed.
  - `check_wt_status.ps1` has no hang fixture, so nothing in the standing suite proves the bound. That is acceptable, and it matches DOCFX4 INFO-2.

## Negtest

Every run used `tools\negtest.ps1 -Preset check -PresetArg tools/<check>.ps1 -Mutations <json> -ExpectPattern <timeout message>` at `7bb65da8a`, from worktree `rvdocfx5`. The doc_hash runs also passed `-Submodules`; without it the baseline SKIPs on ungraded `sub:` citations.

The hung git is a git alias that runs `sleep`, so it keeps the real launcher -> git -> sh -> sleep tree. Survivors were counted as `sleep.exe` processes matching the mutant's sleep length, 3 s after negtest exited.

Every run's baseline passed, the real tree was unchanged, and every copy was removed. Every CAUGHT matched the check's own timeout or assertion line, not a parse error.

| # | Check | Mutation | Verdict | Time | Matched line | Survivors |
|---|-------|----------|---------|------|--------------|-----------|
| B1 | check_build_lock_holder | holder prints `WAITING` instead of `READY` | CAUGHT (exit 1) | 64.9 s | `holder did not print READY within 60 s` | 0 |
| B2 | check_build_lock_holder | B1 + taskkill before the throw removed | CAUGHT (exit 1) | 67.3 s | same as B1; the kill cannot be observed (INFO-7) | 0 |
| W1 | check_wt_status | `WtStatusWalk.Git` args -> `-c alias.zz="!sleep 173" zz`, both 120000 bounds -> 3000 | CAUGHT (exit 1) | 611.9 s | `classification: 'stale_clean' expected STALE_CLEAN got UNKNOWN (git query failed)` | 0 |
| W2 | check_wt_status | W1 + the timeout path returns `ExitCode = 0` and continues | CAUGHT (exit 1) | 433.3 s | same as W1: an empty `rev-parse` still fails closed (INFO-1) | 0 |
| D1 | check_doc_hash_citations | git -> `-c "alias.zz=!sleep 177" zz` (never reads stdin) | MISSED (exit 1 for another reason) | 188.7 s | `git cat-file --batch-check returned 0 lines for 6037 inputs`: the stdin write blocked for the whole hang (LOW-1) | 0 |
| D2 | check_doc_hash_citations | git -> `-c "alias.zz=!cat >/dev/null; sleep 171" zz` (reads stdin, then hangs) | CAUGHT (exit 1) | 129.6 s | `git cat-file --batch-check did not exit within 120 s` | 0 |

## Fix status

- LOW-1 fixed: `Invoke-BatchCheck` writes stdin with `WriteAsync` and bounds it with `Wait(120000)`; on timeout it tree-kills git and throws "did not exit within 120 s (stdin write blocked ...)". Re-run of D1 (git replaced by `!sleep 177`, never reads stdin) by hand in the worktree: fails at 134 s with that message, 0 surviving `sleep`. Not scored by `negtest.ps1`, because its baseline fails on dev for an unrelated reason: `REVIEW_LCDFX13_2026-10-12.md:108` cites a checkbuild directory name that looks like a hash, which does not resolve in the parent repo.
- LOW-2 fixed: `err.Wait(15000)`; a timeout tree-kills git and returns ExitCode -1 with no lines (UNKNOWN, fail closed). Negtest W3 (grandchild holds stderr, bound cut to 2 s) CAUGHT on the UNKNOWN classification assertions.
- INFO-3 fixed (git tree-killed on a throw after start), INFO-5 fixed (taskkill output captured; taskkill and git resolved by full path, PATH entry equal to the cwd skipped). Re-run of W1 (every git call hangs, bound cut to 3 s) CAUGHT on `expected STALE_CLEAN got UNKNOWN`. check_wt_status passes unmutated (129 assertions).
- Not done: INFO-1, 2, 4, 6, 7, 8 (informational).
