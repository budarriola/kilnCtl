# Review: DEVBREAKFX8 (`032909fa1`, four dev check-failure fixes), 2026-10-12

Adversarial review of `032909fa1` on origin/dev ("Fix 4 dev check failures:
push_verify regex literal not path-shaped, reword doc hash-like numbers, host
test count 99, build-gate case 5 ready-file handshake"). Reviewed at dev tip
`b69ba4a94` in a worktree minted from origin/dev. Line numbers are at that tip.

**Verdict: no HIGH, no MED.** All four fixes are correct. Two LOW and four INFO
findings, none of which block the commit. One unrelated INFO: dev tip host tests
are red on `autotune_engine_prestart`.

## Item 1: `tools/check_push_verify.ps1:101` regex `\.` -> `[.]`

`'^\.\s*\(Join-Path \$PSScriptRoot "lib_push_verify_walk[.]ps1"\)$'`: a
one-character class `[.]` matches exactly the literal `.`, same as `\.`. The meaning
is unchanged. Checked directly: old and new patterns give the same result on the real
dot-source line (`tools/push_verify.ps1:60`), a double-spaced variant, `walkXps1`,
`walk\ps1` and `evil.ps1`. `check_source_path_drift.ps1` passes at the tip (288
references, all resolve). `tools/lib_push_verify_walk.ps1` exists.

- **INFO-1** (`tools/check_push_verify.ps1:99-103`, pre-existing). No self-test proves
  that the dot-source branch rejects a foreign dot-source. The `$holes` table (from
  line 180) covers kill surfaces only. If someone appended
  `. (Join-Path $PSScriptRoot "other.ps1")` to the script, the lint would flag it,
  but no assertion pins that behavior.

## Item 2: `docs/audits/REVIEW_TOOLFX13_2026-10-12.md:269`

"`2000000` -> `2000001`" became "2,000,000 -> 2,000,001". The values and the
direction are unchanged, and they still match the guard (`tools/push_verify.ps1:80-81`,
`-gt 2000000`). `check_doc_hash_citations.ps1` passes at the tip.

- **INFO-2.** The row no longer contains the exact literal that the mutation edits
  (`2000000`). A `git grep 2000000` from the source will no longer find this evidence
  row. This is cosmetic. Writing "2e6 -> 2e6+1" or naming `push_verify.ps1:80` would
  keep it searchable.

## Item 3: `firmware/KilnFW/App/test/build_host_tests.ps1:3583-3584` (`$totalExpected` 98 -> 99)

- Counted statically: there are exactly 99 `Invoke-HostTestExe` call sites (lines
  628-3365), all at the same top-level indentation and none conditional. The call
  added for `test_main_reset_fence.c` is at line 2254.
- Measured: the unmutated negtest baseline at `b69ba4a94` printed
  `Built: 99/99 executables`.
- Dropout detection (negtest `h2`, `-NoBaseline`, see table): deleting one
  `Invoke-HostTestExe` call made the script print
  `MISMATCH: 98 executables built but 99 were expected ...` and exit 1
  (line 3617). This is the check's real failing assertion, not a build error.
- **LOW-1** (lines 3613-3615 return before line 3617, pre-existing). The MISMATCH
  comparison runs only when there are no build or run failures. While any executable
  fails, a silent dropout is masked. It shows up only as a `Built: 98/99` line (line
  3603), which nothing asserts on. At this tip one already fails (INFO-4), so the dropout
  guard is not active on dev right now. This is also why `h1` (dropping
  `main_reset_fence`) could not be judged: its baseline failed. `h2` dropped the
  failing executable itself so that the guard could be reached. Fix: print the
  MISMATCH line before the failure exit, as well as on the success path.
- **INFO-3** (lines 3585-3591, pre-existing, by design). With `-Only`,
  `$totalExpected` becomes the selected count, so a dropout cannot be detected
  under `-Only`.

## Item 4: `tools/check_build_gate_reentrant.ps1` case 5 ready-file handshake

Change: the child writes `$ReadyFile` at line 57, only after `WaitOne(0)` has
succeeded (line 51) and after the `exit 2` path (line 56). The parent polls at
lines 127-128 for at most 60 s, and stops early if the holder has exited. It then
asserts the file appeared (line 129). The placeholder argument `"none"` (line 65)
keeps the positional argument non-empty, and the child skips it (line 57).

Negtest (`-Preset check -PresetArg tools\check_build_gate_reentrant.ps1`,
expect `check_build_gate_reentrant: FAIL`). The baseline passed. The real tree was
unchanged and the copies were removed. Every CAUGHT below was confirmed in its log
as an `  FAIL:` assertion line.

| Mutation | Verdict | Failing assertion |
|---|---|---|
| m1 child writes ready file BEFORE `WaitOne` | MISSED | none: the child acquires within ms of writing; the BUSY assertion still saw a held slot (3 s wait) |
| m1b m1 + the hold child sleeps 3 s between write and `WaitOne` | CAUGHT | "slot timeout message matches the BUSY pattern" (the handshake assertion itself said ok) |
| m1c control: correct order + 3 s slow acquire | MISSED (expected; passes) | none. The handshake absorbs a slow child, which the old `Start-Sleep 2` could not |
| m2 parent skips the wait loop | CAUGHT | "holder child acquired the slot (ready-file handshake)" and "BUSY pattern" |
| m2b skip the wait and remove the assert (the old race without the sleep) | CAUGHT | "BUSY pattern" |
| m3 assert removed | MISSED | none (expected: the assert only fires when the child fails to acquire, and then BUSY fails too) |
| h2 (item 3) drop one `Invoke-HostTestExe` call, `-NoBaseline` | CAUGHT | `MISMATCH: 98 executables built but 99 were expected ...` (exit 1) |
| h1 (item 3) drop `main_reset_fence` call | not run | baseline red on `autotune_engine_prestart` (INFO-4) |

- **LOW-2** (lines 125-129). m1 MISSED: if the handshake is inverted, the check does
  not notice under normal timing. The ready-file assertion (line 129) cannot tell
  "acquired" from "about to try". Only the downstream BUSY assertion (line 134)
  catches the inversion, and only when acquisition is slow (m1b). The m3 MISSED
  shows the same thing: line 129 is diagnostic, and BUSY is the real detector. This
  is acceptable, but the comment should say so. A stronger handshake would have the
  child write its `WaitOne` result into the file (for example `got=True`) and have
  the parent assert that content.
- **INFO-5** (lines 125-126). `$ready` is not deleted before `Start-Child`. `$tmpWork`
  is PID-keyed and removed in `finally` (line 146), but a run that is hard-killed
  skips `finally`. A later run that reuses that PID would then find a stale
  `holder_ready`, `New-Item -Force` (line 40) keeps it, and the handshake passes
  vacuously. Fix: `Remove-Item $ready -ErrorAction SilentlyContinue` before line 126.
- **INFO-6** (hang and orphan). There is no new hang risk:
  - The wait loop is bounded at 60 s and exits early on `$holder.HasExited`.
  - The holder's lifetime is bounded by its own `Start-Sleep 8` after a
    non-blocking `WaitOne(0)`.
  - `$holder.WaitForExit()` (line 135) has no timeout. This is pre-existing and
    equally true of `Test-SlotHeld` (line 71). It can hang only if a child
    `powershell` never starts or exits.
  - The `finally` block never kills `$holder`. If the parent dies between lines 126
    and 135, the child is orphaned, but it exits by itself within about 8 s.
  - If the timed `Enter-KilnBuildGate` unexpectedly succeeds, the slot is released at
    once by `Exit-KilnBuildGate` (line 132), and the BUSY assertion fails loud.

  Residual race: none found. The child owns the mutex before the file exists, and
  holds it for 8 s after that. The parent's wait is 2 s (`TIMEOUT_SEC`) plus up to
  1 s of polling, so the margin is about 5 s.

## Unrelated

- **INFO-4.** Dev tip `b69ba4a94` host tests fail: the unmutated baseline reports
  `RUN FAILURES (1): autotune_engine_prestart`, from
  `test_autotune_engine_prestart.c:1426`, `:1584`, `:1596` and `:2371-2377`
  ("autotune_engine_run() must accept a step test on zone 0", the SIMC positive
  control). This is not caused by `032909fa1`. The h2 mutant shows the other 98
  executables pass. The shared main tree carries a staged edit to this test, so
  another session may already be working on it.

## Counts

HIGH 0, MED 0, LOW 2, INFO 6. Negtest: 4 CAUGHT (m1b, m2, m2b, h2), all real
assertions; 3 MISSED (m1, m3 as described, m1c a control expected to pass); 1 not run (h1).
