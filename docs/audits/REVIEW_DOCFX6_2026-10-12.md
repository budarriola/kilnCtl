# Review: docfx6 (dab49f856, d5fadaed4), 2026-10-12

These two commits on origin/dev are the docfx6 fix for `REVIEW_DOCFX5_2026-10-12.md` LOW-1, LOW-2, INFO-3 and INFO-5:

- `dab49f856` is the code commit:
  - `tools/check_doc_hash_citations.ps1` (`Invoke-BatchCheck`): the stdin write is now a `WriteAsync` bounded by `Wait(120000)`. On timeout it runs `taskkill /T /F` and throws.
  - `tools/wt_status.ps1` (`WtStatusWalk.Git`): `err.Wait(15000)` is bounded, and a timeout returns ExitCode -1 with no lines. A throw after start now tree-kills git. taskkill's output is captured, and taskkill and git are resolved by full path.
- `d5fadaed4` is the fix-status note in `REVIEW_DOCFX5_2026-10-12.md`.

Counts: HIGH 0, MED 0, LOW 2, INFO 6.

**Answers to the four questions:**

1. **Can a bound misclassify a healthy-but-slow repo in an unsafe way?** No. Neither bound can cause a wrong prune or a false PASS.
   - In wt_status, every timeout gives ExitCode -1. A -1 from `log`, `status --ignored`, `rev-parse --absolute-git-dir` or `status v2` sets `GitOk=false`, which gives UNKNOWN. A -1 from `cherry` marks every ahead commit unlanded, which gives HAS_WORK.
   - Unregistered directories make no git calls, so the bound cannot affect ORPHAN_DIR.
   - Prune re-runs `$gather` and re-reads the process list for each directory before it removes anything.
   - The worst a slow repo can do is make every registered worktree UNKNOWN, so nothing is pruned (INFO-2).
   - In check_doc_hash_citations, a slow write throws, and the check exits 1 (D1). It never exits 0 or 3.
2. **Is the kill scoped to the started git process?** Yes, and it never reaches unrelated processes. On the err-timeout path, though, it reaches nothing at all (LOW-1).
3. **Is the git resolution from PATH sound?** Yes, for the case the fix targets, including skipping the current directory. Edge cases are listed in INFO-3, and check_doc_hash_citations still starts a bare `git`.
4. **Are W1 and W3 meaningful?** W1 is a regression check of the stdout bound only. W3 does not show that the new bound works (INFO-1). This review's N1/N2 pair does.

## LOW

- **LOW-1** (`tools/wt_status.ps1`, the `!err.Wait(15000)` branch, and the `catch { KillTree(p); throw; }` path once git has exited): on the err-timeout path the kill does nothing, so the grandchild that holds stderr keeps running.
  - That branch is reached only after `outT` has completed and `p.WaitForExit` has returned, so the launcher git has already exited.
  - `taskkill /T /F /PID <exited pid>` answers `ERROR: The process "<pid>" not found.` A process that has exited is gone from the process snapshot even while the open .NET handle keeps its PID reserved. taskkill therefore never walks to the orphaned grandchild.
  - **Measured by hand.** The script loads the `WtStatusWalk` type from `wt_status.ps1` and calls `Git(dir, "-c \"alias.zz=!sleep 53 >/dev/null & git\" zz rev-parse HEAD")`.
    - It returned in 16 s with ExitCode -1 and no lines, which is correct and fails closed.
    - 2 s later the `sleep 53` was still running. Its ParentProcessId was the exited launcher's PID.
    - A direct `taskkill /T /F /PID` on the exited launcher printed "not found" and left its `sleep 47` grandchild running.
  - **Consequence: a leak only.** The grandchild runs to its natural end, and the classification is still UNKNOWN. The new comment "treat as a failed git call" is accurate. The kill is not: the fix-status note implies W3 shows a tree-kill, and it does not.
  - Fix, optional: start git inside a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. That reaches descendants whatever the parent's state. Or document that the kill is a no-op on this path.
  - A long-lived grandchild such as a daemon may be meant to outlive git, so leaving it alone is defensible.
- **LOW-2** (`d5fadaed4`, `docs/audits/REVIEW_DOCFX5_2026-10-12.md:86`): the fix-status note quoted the LCDFX13 checkbuild directory hash as a bare citation. check_doc_hash_citations therefore failed on two lines at `d5fadaed4`, not one.
  - At `d5fadaed4` the unmutated check printed `FAIL: 2 cited hash(es) do not resolve`, the DOCFX5 line and the LCDFX13 line, in 96.6 s.
  - **Both are fixed on dev now:** `859bf2a9a` (DOCFX5) and `243f90f00` (LCDFX13). Neither citation is present at the tip this review lands on.
  - Lesson for fix notes: describe a non-resolving hash in words, or prefix it with the `fabricated` marker. Never quote it bare.

## INFO

- **INFO-1** (the fixer's W1/W3): neither of them shows that the new stderr bound is live.
  - W1 hangs every git call, so it exercises the `outT` 120 s path. That is the DOCFX5 bound, re-run as a regression after the try/catch restructure, and it is useful for that.
  - W3's grandchild produces no stdout. Without the bound, `err.Wait()` would block until the grandchild exits, then return exit 0 with empty output. The empty `rev-parse` then sets `GitOk=false`, so the result is UNKNOWN either way. The fixer said so.
  - This review's pair shows the bound is live:
    - **N1:** each git call runs the real command, while a backgrounded `sleep 25` keeps stderr open. CAUGHT on `expected STALE_CLEAN got UNKNOWN (git query failed)`.
    - **N2:** the same injection with the bound removed. No classification assertion failed; the 4 failures it did report come from INFO-6. So the UNKNOWN in N1 is caused by the bound and nothing else.
- **INFO-2** (wt_status, Q1): there is a cost, but it fails in the safe direction.
  - Suppose a repo config made git leave a stderr-inheriting grandchild on every call, such as a daemon started without handle redirection. Then every registered worktree would read UNKNOWN, each call would cost 15 s, and `-Prune` would remove only ORPHAN_DIRs.
  - That was not seen on this machine (the baseline passed in 117 s). If it ever is, it shows up as "git query failed" on every row.
- **INFO-3** (`WtStatusWalk.GitExe`, Q3): the resolution is sound for the stated purpose. CreateProcess searches the parent process's current directory, and `Directory.GetCurrentDirectory()` is that same directory, so skipping it closes the right hole. The edge cases:
  - **Fallback.** If no PATH entry holds `git.exe`, the fallback is a bare `"git"`, which brings back the current-directory search. That is acceptable, because no git on PATH fails anyway.
  - **Relative PATH entries.** Only an entry equal to the current directory is skipped. A relative entry such as `tools` still resolves under it, the same as CreateProcess would.
  - **Quoted PATH entries.** `"C:\Program Files\Git\cmd"` with quotes makes `GetFullPath` throw on .NET Framework, so the entry is silently skipped.
  - **Only `git.exe` is looked for.** If an earlier PATH directory holds `git.cmd`, PowerShell's own `git` calls in the same script (`Get-Registered`, `worktree prune`) would resolve differently from `WtStatusWalk`.
  - **check_doc_hash_citations is not covered.** It still runs `Process.Start` with `FileName = 'git'`, so the current-directory search that DOCFX5 INFO-5 described remains there. INFO-5 was fixed only in wt_status.
- **INFO-4** (`tools/check_doc_hash_citations.ps1:337-341`):
  - **Message.** The write-timeout message says "did not exit within 120 s" when it is the write that timed out. The worst case is 120 + 120 s.
  - **Early exit.** A git that exits early without reading stdin makes the write task fault, and `.Wait` throws an AggregateException instead of returning false. The check still exits nonzero.
  - **Abandoned write.** After the kill, the pending `WriteAsync` task is never observed. That is harmless on .NET 4.5 and later.
  - **Kill scope.** `& $killTree` reads `$p` through dynamic scope from the function, and git is still alive on that path. The kill reaches it: D1 left 0 survivors.
- **INFO-5** (kill scope, Q2): the kill never targets anything but the started process's PID.
  - That PID cannot be reused while the .NET `Process` object is open. Both `KillTree` call sites run inside the `using`, and `$p` is never disposed in check_doc_hash_citations.
  - On a live root, `/T` still has the DOCFX5 INFO-4 caveat: children are matched by ParentProcessId with no creation-time check.
  - On an exited root, taskkill kills nothing (LOW-1). It does not fall through to other processes.
- **INFO-6** (`tools/check_wt_status.ps1:132,145`): the fixture's holder processes are `Start-Sleep -Seconds 600`.
  - A wt_status pass that runs longer than about 10 minutes sees them exit. It then correctly reclassifies and prunes `proc_wt`/`pfx_longer`, and the check reports `'proc_wt' must survive`.
  - That is what produced N2's 4 failures (3054 s run). It is a fixture timing limit, not a wt_status defect.
  - It matters only for slow-git negtests like N1/N2. Score those on the classification lines, as done here.

## Negtest

Every run used `tools\negtest.ps1 -Preset check -PresetArg tools/<check>.ps1 -Mutations <json> -ExpectPattern <pattern>`, from worktree `rvdocfx6`. The doc_hash runs also passed `-Submodules`.

Both runs were based on a local, unpushed scratch commit on top of `d5fadaed4`. It added the `fabricated` marker to the two stale citations of LOW-2, so the unmutated baseline could pass, and it has been discarded.

Both baselines passed: doc_hash in 47 s, wt_status in 117.2 s. Both runs reported `real_tree_unchanged: true` and `copies_removed: true`. Survivors were counted as `sleep.exe` processes with the mutant's sleep length, after the run.

| # | Check | Mutation | Verdict | Time | Matched / observed | Role |
|---|-------|----------|---------|------|--------------------|------|
| D1 | check_doc_hash_citations | `$psi.Arguments` -> `-c "alias.zz=!sleep 177" zz` (git never reads stdin) | CAUGHT (exit 1) | 132.2 s | `git cat-file --batch-check did not exit within 120 s (stdin write blocked; repo: '')`; 0 `sleep 177` survivors | proves LOW-1 (DOCFX5) fixed |
| D1r | check_doc_hash_citations | D1 + the write reverted to a synchronous `StandardInput.Write` | MISSED (exit 1 for another reason) | 189.3 s | `returned 0 lines for ... inputs` after the whole hang | control: D1 discriminates |
| N1 | check_wt_status | each git call -> `-c "alias.zz=!sleep 25 >/dev/null & git" zz <args>` (real output; a grandchild holds stderr 25 s) | CAUGHT (exit 1) | 1189.2 s | `classification: 'stale_clean' expected STALE_CLEAN got UNKNOWN (git query failed)` (31 of 127 assertions failed) | proves the 15 s err bound fires and fails closed |
| N2 | check_wt_status | N1 + `err.Wait(15000)` -> `err.Wait(-1)` | MISSED (exit 1 for another reason) | 3053.6 s | no classification failure; 4 prune failures from the fixture's 600 s holders expiring (INFO-6) | control: N1's UNKNOWN comes from the bound |

Every CAUGHT came from a mutant that ran and failed on the check's own assertion or timeout message. Neither was a parse or compile error: PowerShell and C# compile at run time, and both mutant logs show the fixture or the batch running before the matched line.

The hand measurement behind LOW-1 is described there. It was run outside negtest, because no standing check can observe the grandchild.
