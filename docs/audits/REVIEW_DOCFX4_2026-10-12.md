# Review: docfx4 (872348b7c), 2026-10-12

Commit `872348b7c` on origin/dev fixes two REVIEW_DOCFX3 items:

- LOW-1: `tools/check_build_gate_usage.ps1` now starts the git ls-files stdout read with `ReadToEndAsync()` and bounds it with `Wait(120000)`. On a timeout it kills the process, prints a FAIL line and exits 1.
- INFO-4: in `tools/check_push_verify.ps1`, `$pvCs = ''` now sits on its own line.

The fixer's only negtest was `Wait(120000)` -> `Wait(0)`. That proves the FAIL branch can fire, but not that a real hang is bounded. This review ran a hang simulation at `828db5360` (origin/dev tip, which contains 872348b7c). The baseline passed, the real tree was unchanged, and the copy was removed.

Counts: HIGH 0, MED 0, LOW 1, INFO 5.

## LOW

- **LOW-1** (FIXED: both timeout branches now `taskkill /T /F` with distinct messages, same tree-kill and bounded reads added to check_build_lock_holder, check_doc_hash_citations, wt_status; negtest hang mutant CAUGHT at the 120 s message with no `sleep 180` survivor) (`tools/check_build_gate_usage.ps1:40`): on a timeout, `$proc.Kill()` kills only Git for Windows' launcher. The process that actually hung keeps running.
  - `git` resolves to `C:\Program Files\Git\cmd\git.exe`, which is a launcher. It spawns `mingw64\bin\git.exe`, and that process does the work. .NET Framework's `Process.Kill()` (PS 5.1 has no `Kill($true)`) is not tree-wide.
  - Measured by hand: `git -c "alias.zz=!sleep 40" zz` was started through the same ProcessStartInfo shape, and `Kill()` was called after a 2 s `Wait`. The launcher died. `mingw64 git.exe`, `sh.exe` and `sleep.exe` all stayed alive, and the stdout task stayed incomplete.
  - The check itself is still bounded, because it calls `exit 1` straight after the kill and does not wait on the orphan. A really hung `git ls-files` would therefore be left running indefinitely after a FAIL. It holds the check's inherited stderr handle and its cwd inside the worktree. run_all_checks.ps1 redirects check output to files (line 765) and does not read a pipe, so the orphan cannot stall the suite. It can, however, keep a worktree directory busy for a later remove.
  - Negtest N3 (Kill removed) is CAUGHT with exactly the same output as N1. The kill has no effect the check can observe.
  - Fix (optional): kill the process tree. One way is `taskkill /PID <id> /T /F`, which run_all_checks.ps1:886 already uses. Another is to resolve `mingw64\bin\git.exe` up front so that there is no launcher layer.

## INFO

- **INFO-1** (`tools/check_build_gate_usage.ps1:39-44`): apart from LOW-1, the timeout path is bounded end to end.
  - The read starts before any wait. `Wait(120000)` bounds the read, and a timeout exits at once with no `WaitForExit` after the kill, so nothing can hang there.
  - If the read finishes, `WaitForExit(120000)` bounds the gap until the process exits. Line 44 then checks `ExitCode` and the listed count.
  - Stderr is not redirected, so it is inherited, and no unread second pipe can fill up and deadlock. Stdin is inherited too; `ls-files` does not read it.
  - If the read task faults, `Wait` throws an AggregateException rather than returning false. Under `$ErrorActionPreference = "Stop"` that ends the script with a nonzero exit, so it cannot pass silently.
  - `Task.Wait(int)`, `ReadToEndAsync` and `.Result` are all available on .NET 4.5+ under PS 5.1.
  - Nit: the worst case is 240 s (120 s for the read plus 120 s for the exit), but both branches print the same "within 120 s" text, so a reader cannot tell which wait expired.
- **INFO-2** (negtest): the hang simulation shows the read is now bounded, and that the synchronous form is not.
  - N1 swaps in a hanging git (`git -c "alias.zz=!sleep 180" zz`). That keeps the real launcher -> git -> sh -> sleep tree and holds stdout open with no output. The check printed the FAIL line at 122 s.
  - N2 is the same hang with the fix reverted to a synchronous `ReadToEnd()`. It blocked for the full hang (183 s), then failed on a different line ("failed under ... cannot enumerate scripts"). An unbounded read therefore takes as long as the hang does.
  - Every CAUGHT is the real `FAIL: git ls-files did not exit within 120 s` line with exit 1, not a parse error. All three mutants parsed and ran.
  - The check has no standing test that a hang is bounded. That is acceptable for a 3-line bound, and it matches DOCFX3 INFO-2.
- **INFO-3** (`tools/check_push_verify.ps1:96-97`): fixed.
  - Line 97 `$pvCs = ''` is now a real assignment, and the Assert on line 96 gets only its two arguments.
  - The check passes directly (`check_push_verify: all cases passed`, exit 0, 59 s), and `check_build_gate_usage.ps1` passes directly (21 callers, exit 0).
  - Conflict risk: toolfx16 is editing the pin block at lines 82-96. Line 96 is the last line of that block and is adjacent to the changed line 97, so a toolfx16 branch based before `872348b7c` will probably get a textual rebase conflict there. Resolve it by keeping both lines.
  - The pin hash (`$PvPinSha256`) covers only `push_verify.ps1` and its lib, not this check, so this change does not need a pin update.
- **INFO-4** (same pattern elsewhere; not fixed): other places that read a child-process pipe or wait on one with no bound.
  - `tools/check_build_lock_holder.ps1:41` loops on `while (-not EndOfStream) { ReadLine() ... READY }` with no bound. A holder child that blocks before it prints READY would hang the check.
  - `tools/check_doc_hash_citations.ps1:330-338` uses async stdout/stderr reads, so it cannot deadlock. But `$p.WaitForExit()` and `.Result` have no bound, so a hung `git cat-file --batch-check` hangs the check.
  - `tools/wt_status.ps1:94-98` (a tool, not a check) reads stdout synchronously with `ReadLine` and then calls `WaitForExit()` with no bound.
  - The two remaining `ReadToEnd()` hits under tools/ (`check_wait_for.ps1:34`, `negtest.ps1:476`) read files, not pipes, so they are not affected.
- **INFO-5**: REVIEW_DOCFX3's LOW-1/INFO-4 "FIXED" annotations are accurate, apart from the process-tree caveat in LOW-1 above.

## Negtest table

Runs used `tools\negtest.ps1 -Preset check -PresetArg tools\check_build_gate_usage.ps1 -ExpectPattern 'FAIL: git ls-files did not exit within 120 s'` at base `828db5360`. Logs are in `%TEMP%\negtest_logs\20261012_083447_g7in`. The baseline passed (11.4 s), `real_tree_unchanged` was true, and `copies_removed` was true.

| # | Mutation | Verdict | Time | Matched line |
|---|----------|---------|------|--------------|
| N1 | `$psi.Arguments` -> `-c "alias.zz=!sleep 180" zz` (hung git, stdout held open) | CAUGHT (exit 1) | 122.3 s | `FAIL: git ls-files did not exit within 120 s` |
| N2 | N1 + async read/Wait/Result reverted to `$gitOut = $proc.StandardOutput.ReadToEnd()` | MISSED (expected; exit 1 for another reason) | 183 s | `FAIL: git ls-files ... failed under ... cannot enumerate scripts`: the read was unbounded and took as long as the hang |
| N3 | N1 + `try { $proc.Kill() } catch { };` removed | CAUGHT (exit 1) | 124.5 s | same as N1: the kill cannot be observed (LOW-1) |
