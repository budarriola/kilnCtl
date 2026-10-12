# Review: docfx3 (ff4b2f78c), 2026-10-12

Commit `ff4b2f78c` on origin/dev marks three REVIEW_DEVBREAKFX8 items fixed:

- INFO-1 is closed as not applicable. The commit says toolfx15's SHA-256 pin (`3c806102c`) covers an appended dot-source.
- INFO-2 changes the REVIEW_TOOLFX13 evidence row to cite `-gt 2000000` -> `-gt 2000001` (push_verify.ps1:80).
- INFO-6 adds a 120 s `WaitForExit` bound that fails loud. It covers `tools/check_build_gate_reentrant.ps1` (the probe and holder children) and `tools/check_build_gate_usage.ps1` (git ls-files).

The fixer did not negtest. This review ran three `tools\negtest.ps1 -Preset check` runs at ff4b2f78c. In each, the baseline passed and the real tree was unchanged.

Counts: HIGH 0, MED 0, LOW 1, INFO 5.

## LOW

- **LOW-1** (`tools/check_build_gate_usage.ps1:37-38`): the new bound does not cover the hang it is meant to catch.
  - Line 37 calls `$proc.StandardOutput.ReadToEnd()`, which has no bound. ReadToEnd returns only at stdout EOF, and stdout normally closes when git exits. The bounded `WaitForExit(120000)` on line 38 therefore covers only the short gap between EOF and process exit.
  - Failure scenario: git stalls while holding stdout open, for example on a OneDrive or antivirus filesystem stall while it walks the tree. The check then hangs at line 37 forever and never reaches the FAIL line.
  - Negtest evidence: mutant U1 changes the bound to `WaitForExit(0)` and is MISSED (exit 0). By the time ReadToEnd returns, git has already exited, so even a 0 ms wait succeeds and the timeout branch cannot be reached.
  - The INFO-6 "FIXED ... fail loud" note in REVIEW_DEVBREAKFX8 overstates this file. The reentrant check is genuinely fixed (see INFO-1).
  - Fix: bound the read itself. For example, use `$t = $proc.StandardOutput.ReadToEndAsync()`, then `if (-not $t.Wait(120000)) { Kill; FAIL }`, and then `WaitForExit()`. Alternatively, redirect git's output to a temp file and wait on the process.

## INFO

- **INFO-1** (`tools/check_build_gate_reentrant.ps1:71`, `:140`): correct.
  - The `WaitForExit(120000)` return value is checked, the child is killed on timeout, and a throw reaches the outer `catch` (line 143). That catch records `unexpected error: ...` and exits 1. The message does not match the BUSY pattern, so run_all_checks.ps1 files the result as FAIL, not BUSY.
  - Reading ExitCode after a bounded wait is safe here, for two reasons. `Start-Child` redirects no streams, so there is no async-flush gap that would need a parameterless `WaitForExit()`. It also touches `$p.Handle` (line 66), which avoids the PS 5.1 `Start-Process -PassThru` null-ExitCode issue.
  - `Kill()` is not tree-wide, but neither the probe child nor the holder child has children of its own. If a killed holder abandons its mutex, the OS releases it.
  - In check_build_gate_usage.ps1, ExitCode after a synchronous ReadToEnd is likewise safe, since no async handlers are used.
  - Negtests R1 (probe bound 0 ms), R2 (holder bound 0 ms) and R3 (probe return value inverted) are all CAUGHT on the new `FAIL: unexpected error: ... did not exit within 120 s` line.
- **INFO-2** (control mutant R4): replacing the holder bound with the old bare `$holder.WaitForExit()` is MISSED, as expected. The check cannot detect that its own bound was removed, because no case makes a child hang. This is not worth a fixture.
- **INFO-3** (DEVBREAKFX8 INFO-1 claim): verified, with one wording nit.
  - Negtest P1 adds a second dot-source line to `tools/push_verify.ps1:60`. Negtest P2 adds `. (Join-Path $PSScriptRoot "other.ps1")` to `tools/lib_push_verify_walk.ps1`. Both are CAUGHT by `FAIL: L1: push_verify.ps1 + lib_push_verify_walk.ps1 match the pinned kill-surface hash`.
  - The wording nit: the assert that catches an appended dot-source is the pin-equality assert (`tools/check_push_verify.ps1:95`). The "hash changes when the script changes" assert (`:96`) only self-tests hash sensitivity, and it appends a `$gitExe` line, not a dot-source.
  - The cited `check_push_verify.ps1:99-103` now points at the LOW-2/INFO-3 asserts. That is acceptable, because the item is marked not applicable.
- **INFO-4** (`tools/check_push_verify.ps1:96`, pre-existing from `3c806102c`): `$pvCs = ''` is glued onto the end of the L1 Assert line.
  - A PS 5.1 test shows the parse: `Assert $true "msg"$pvCs = ''` passes `$pvCs`, `=` and `''` as three extra `$args`, and `$pvCs` is never assigned.
  - This is harmless today. Line 98's `$pvCs += ...` starts from `$null`, so it builds the same string. However, the intended reset is dead code: if `$pvCs` were ever set before line 96, stale text would leak into the LOW-2/INFO-3 regex asserts.
  - Fix: move the assignment onto its own line.
- **INFO-5** (`docs/audits/REVIEW_DEVBREAKFX8_2026-10-12.md:29`, `:33-34`): the INFO-2 row is correct. `tools/push_verify.ps1:80` is `if ($FetchTimeoutSec -gt 2000000) {`, and the TOOLFX13 row (`REVIEW_TOOLFX13_2026-10-12.md:269`) quotes it exactly.
  - However, the fix also rewrote the DEVBREAKFX8 review's own narrative so that it no longer contains the literal. It now reads "the cap mutation (2e6 -> 2e6+1, in digits) became ...", and later "(the digit string)".
  - That changes a historical quote of the before-text for no functional reason. No check greps for the literal. This is cosmetic.

## Negtest table

All three runs are at ff4b2f78c. The baselines passed: reentrant 34.9 s, usage 20.9 s, push_verify 131.3 s. Every CAUGHT below is the check's own `FAIL:` assertion line. These are PowerShell checks, so there is no compile step.

| Check | Mutation | Verdict | Matched line |
|---|---|---|---|
| check_build_gate_reentrant | R1 probe `WaitForExit(120000)` -> `(0)` | CAUGHT | `FAIL: unexpected error: check_build_gate_reentrant: probe child did not exit within 120 s` |
| check_build_gate_reentrant | R2 holder `WaitForExit(120000)` -> `(0)` | CAUGHT | `FAIL: unexpected error: check_build_gate_reentrant: holder child did not exit within 120 s` |
| check_build_gate_reentrant | R3 probe `if (-not $p.WaitForExit(...))` -> `if ($p.WaitForExit(...))` | CAUGHT | `FAIL: unexpected error: ... probe child did not exit within 120 s` |
| check_build_gate_reentrant | R4 control: holder line restored to bare `$holder.WaitForExit()` | MISSED (expected) | INFO-2 |
| check_build_gate_usage | U1 `WaitForExit(120000)` -> `(0)` | MISSED | LOW-1: git has already exited when ReadToEnd returns |
| check_build_gate_usage | U2 `-not` dropped | CAUGHT | `FAIL: git ls-files did not exit within 120 s` |
| check_push_verify | P1 second dot-source line in push_verify.ps1 | CAUGHT | `FAIL: L1: push_verify.ps1 + lib_push_verify_walk.ps1 match the pinned kill-surface hash ...` |
| check_push_verify | P2 foreign `. (Join-Path $PSScriptRoot "other.ps1")` in lib_push_verify_walk.ps1 | CAUGHT | same L1 line |
