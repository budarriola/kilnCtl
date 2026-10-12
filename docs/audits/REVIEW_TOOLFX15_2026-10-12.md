# Review: toolfx15 (push_verify kill-surface pin), 2026-10-12

Scope: origin/dev `3c806102c` (check_push_verify: SHA-256 pin over `push_verify.ps1` +
`lib_push_verify_walk.ps1` replaces the AST deny-list, TOOLFX14 LOW-1/2/3; `land.ps1` push-failure
text) and `888b3a27c` (fix marks in `REVIEW_TOOLFX14_2026-10-12.md`). Reviewed at dev tip
`1142b1899` in a fresh worktree.

**Severity counts: 0 HIGH, 0 MED, 3 LOW, 2 INFO.**

## Evidence

- Targeted checks, run directly in the worktree: `check_push_verify` rc 0 (49 ok, pin line prints
  the committed hash), `check_land` rc 0, `check_doc_citations` rc 0, `check_worktree_mint` rc 0,
  `check_commit_guard` rc 0. `check_doc_hash_citations` rc 1, but the three failures all sit in
  `docs/audits/REVIEW_DEVBREAKFX8_2026-10-12.md` (`4d4ac698d`, backticked numbers such as 2000000
  read as hashes). They are unrelated to toolfx15. No check greps the deleted
  `Get-KillLintViolations`/`Get-LintCount` names or the old land.ps1 text. The pin is a 64-hex
  string, so the 7..40-hex citation pattern does not match it.
- The pin was recomputed independently in Python from `git show HEAD:` (LF in the index) as
  sha256(pv + "\n--LIB--\n" + lib). It equals the committed `$PvPinSha256`. Both files are pure
  ASCII with no BOM, and every CR is part of a CRLF (295/295 and 55/55).
- `tools\negtest.ps1 -Preset check -PresetArg check_push_verify.ps1 -Mutations <json> -ExpectPattern "FAIL: "`
  ran 12 mutations, 3 copies in parallel. The baseline PASSED, `real_tree_unchanged` true,
  `copies_removed` true. In the "rubber-stamp" mutations the pin line is also changed to the
  mutant's hash (computed offline), which is what an editor who pastes the printed hash would do.
  Every log was inspected. Each CAUGHT has exactly one `  FAIL: <assert>` line, a final
  `check_push_verify: 1 FAILED` line and no parse error. In every rubber-stamp mutant the pin
  assert reads `ok`, so the pin edit was correct and the C# still compiled. Each MISSED ends with
  `all cases passed`.

| Mutation | Verdict | Failing assertion |
|---|---|---|
| M1 dead `Stop-Process -Id $fetchPid` after KillProc, pin untouched | CAUGHT | L1 pin mismatch |
| M2 same dead Stop-Process, pin rubber-stamped | **MISSED** | none (LOW-1) |
| M3 pin bypass: `Assert ($PvPinSha256 -eq $PvPinSha256)` | **MISSED** | none (INFO-1) |
| M4 pin bypass + dead Stop-Process | **MISSED** | none (INFO-1) |
| M5 file order swapped in Get-PvSurfaceHash | CAUGHT | L1 pin mismatch |
| M6 CRLF normalisation removed | CAUGHT | L1 pin mismatch |
| M7 lib path made missing (`$pvLibSrc` = $null) | CAUGHT | L1 pin mismatch |
| M8 C# `ResumeThread` result unchecked, pin rubber-stamped | CAUGHT | LOW-2 ResumeThread/CreateFileW |
| M9 `CreateProcessW(null, ...)`, pin rubber-stamped | CAUGHT | INFO-3 CreateProcessW(gitExe |
| M10 `KillProc` body emptied, pin rubber-stamped | CAUGHT | runtime "root-only fallback: the root git itself is killed (KillProc)" |
| M11 C# `OpenProcess` extern added, pin rubber-stamped | **MISSED** | none (LOW-1) |
| M12 Get-PvSurfaceHash returns the pin constant | CAUGHT | "L1: the hash changes when the script changes" |

## Answers

**Q1, pin soundness.** Sound for these files. CRLF→LF normalisation makes the hash independent of
checkout line endings (`.gitattributes` forces `*.ps1 eol=crlf`; M6 shows the normalisation is
load-bearing). The separator plus fixed file order is enforced (M5). A missing file gives
`$null`, so the hash does not match and the check FAILS instead of passing (M7). The self-assert at
`tools/check_push_verify.ps1:96` is real but narrow: it proves the hash function depends on its
input (M12), and it does not prove that the line 95 assert compares the computed hash (M3). See
INFO-1 and INFO-2 for two small caveats.

**Q2, rubber-stamping.** Yes, the pin is easy to rubber-stamp. See LOW-1.

**Q3, lost coverage.** Deleting `Get-KillLintViolations` also removed the *positive* pins that a
hash bump cannot satisfy by accident:
- exactly one Add-Type;
- exactly one `[PvJob]::Launch($gitExe, ...)`;
- `[PvJob]::Kill($fetchJob)` and `[PvJob]::KillProc($fetchH)` exactly once;
- `.Kill()` only in Stop-PvTargets and `GetProcessById` only in Open-PvTargets;
- the C# TerminateProcess call sites pinned to `(pi.hp,1)|(h,1)` and TerminateJobObject to `(j,1)`;
- the KillProc body exact, and the Launch catch path terminating its own `pi.hp`;
- no OpenProcess/GetProcessById/EntryPoint/DuplicateHandle in the C#.

The deny-list half (forbidden cmdlet names) was the bypassable part. Dropping it is fine. M2 and
M11 now pass after a hash bump, and the old lint would have caught both. The kept asserts are
non-vacuous: ResumeThread/CreateFileW (M8) and CreateProcessW(gitExe (M9) each fail on a
rubber-stamped mutant.

**Q4, collateral.** Nothing else broke. See Evidence. The land.ps1 edit at `tools/land.ps1:403-404`
is correct. The old `}    Finish 1 ...` line ran Finish only after the race `if` anyway, so behaviour
is unchanged and only the text changed. The message now matches the three-way `Get-PushRejectVerdict`
(refused / race / other).

## Findings

### LOW-1: the pin is rubber-stampable and no longer backed by positive structural pins
`tools/check_push_verify.ps1:95` prints the exact value to paste ("then set `$PvPinSha256 to
<hash>"). The only instruction to review the kill surface is the comment at lines 82-86. No doc
mentions the pin: `docs/MCP_SERVERS.md` "Git workflow guards", `docs/agent_rules/COMMON.md` and
the land flow are all silent. Every ordinary edit to push_verify.ps1 (usage text, a new refusal)
forces a bump, so the bump becomes routine. M2 (dead Stop-Process) and M11 (C# OpenProcess)
pass once the pin is bumped.
Fix, narrowest stronger option: keep the pin, and also restore the positive half of the old lint as
a short list of exact-count asserts. These are not a deny-list and have nothing to bypass:
- one Add-Type;
- one `[PvJob]::Launch($gitExe,`, with `$gitExe` assigned exactly once from `Get-Command git -CommandType Application`;
- `[PvJob]::Kill($fetchJob)` x1 and `[PvJob]::KillProc($fetchH)` x1;
- the C# TerminateProcess/TerminateJobObject call-site pins;
- no `OpenProcess|GetProcessById|DuplicateHandle|EntryPoint` in the C# block.

Also tie a bump to a review: require the pin line to carry `# reviewed: docs/audits/<file>.md`, and
have the check assert that this file exists and contains the 64-hex pin. Add one line under "Git
workflow guards" in MCP_SERVERS.md.

### LOW-2: line-join defect at `tools/check_push_verify.ps1:96`
`... "L1: the hash changes when the script changes"$pvCs = ''` puts the old `$pvCs = ''` line on
the end of the Assert line. This is the same defect class this commit fixed in land.ps1. PowerShell
appends `$pvCs` (undefined at that point) to the message and passes `=` and `''` to `Assert` as
extra `$args`, which are ignored. `$pvCs` is therefore never initialised. The code works only
because `$null += "str"` produces a string. Under `Set-StrictMode`, or with an earlier `$pvCs`,
this breaks or carries stale C# text into the LOW-2/INFO-3 asserts. Fix: put `$pvCs = ''` back on
its own line.

### LOW-3: `888b3a27c` wrote literal TAB characters into the review doc
`docs/audits/REVIEW_TOOLFX14_2026-10-12.md:196` reads `in <TAB>ools/check_push_verify.ps1` and
`<TAB>ools/land.ps1`, because a `\t` escape was expanded. Fix: replace them with `tools/`.

### INFO-1: no meta-test for the pin comparison
M3/M4: changing line 95 to compare the pin with itself goes unnoticed. The self-assert at line 96
compares against `$PvPinSha256`, not `$pvHash`, so it cannot detect this. Any check can be
defeated by editing it, and a reviewer of the check's diff would see this edit, so this is
informational. A cheap guard is `Assert ($pvHash -ne (Get-PvSurfaceHash ($pvSrc + 'x') $pvLibSrc))`
plus an AST assert that line 95's left operand is `$pvHash`.

### INFO-2: hash framing and encoding caveats
- The separator has no length prefix. Text containing "\n--LIB--\n" could move across the file
  boundary without changing the hash. Neither file contains it, and the moved code would still be
  in pinned content, so this is harmless.
- `Get-Content -Raw` decodes with the ANSI code page on PS 5.1 and UTF-8 on pwsh 7, and it strips a
  BOM. Both files are pure ASCII today, so the hash is host-independent now. A non-ASCII character
  added later would make the pin depend on the host. Hashing `[IO.File]::ReadAllBytes` after
  CRLF→LF on bytes would remove that dependence.

## Fix status

Open: LOW-1, LOW-2, LOW-3, INFO-1, INFO-2 (pooled for a fixer).
