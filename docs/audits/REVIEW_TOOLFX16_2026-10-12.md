# Review: toolfx16 (push_verify kill-surface exact-count asserts), 2026-10-12

Scope: origin/dev `8381e658a` (fixes for `REVIEW_TOOLFX15_2026-10-12.md`). The commit adds K1-K13 to
`tools/check_push_verify.ps1`, puts `# reviewed: <doc>` on the pin line, adds a paragraph to
`docs/MCP_SERVERS.md` and fixes the TABs in `REVIEW_TOOLFX14`. The glued `$pvCs = ''` was fixed
separately in `872348b7c` and is not reviewed here. Reviewed at dev tip `e838a9095` in a fresh worktree.

**Severity counts: 0 HIGH, 0 MED, 2 LOW, 4 INFO.**

## Evidence

- `check_push_verify.ps1` run directly in the worktree: rc 0, 63 `ok`, `all cases passed`. K1-K13 all
  pass against the current `push_verify.ps1` and `lib_push_verify_walk.ps1`, so there is no false
  positive today. The full-line comment strip in K7 is load-bearing: `push_verify.ps1:106` is a
  comment that contains `taskkill`.
- The `REVIEW_TOOLFX14` TAB fix is correct: the file now has 0 TAB characters. The MCP_SERVERS.md
  paragraph (`docs/MCP_SERVERS.md:753`) matches what the check does.
- I ran `tools\negtest.ps1 -Preset check -PresetArg tools\check_push_verify.ps1 -Mutations <json> -ExpectPattern "FAIL: " -Parallel 4`
  with 18 mutations. The baseline passed, `real_tree_unchanged` was true and `copies_removed` was true.
  In each "stamped" mutation, the pin line was changed to the mutant's hash, computed offline, and
  that hash was appended to the reviewed doc. This is what a rubber-stamping bumper would do. I
  checked every log. In each stamped mutant the L1 pin assert and K13 read `ok`, and the
  push_verify run cases still pass, so the stamp was correct and the mutant parsed. Each CAUGHT
  mutation fails on a real `  FAIL: K<n>` assertion line, with a final `check_push_verify: N FAILED`
  line and no parse error. Each MISSED mutation ends with `all cases passed`. Every injected kill is
  wrapped in `if ($false)`, so it never runs.

| Mutation | Verdict | Failing assertion |
|---|---|---|
| B1 `stop-process -Id $fetchPid` (lowercase), stamped | **MISSED** | none (LOW-1) |
| B2 `spps -Id $fetchPid` alias, stamped | **MISSED** | none (LOW-2) |
| B3 `[pvjob]::kill($fetchJob)` second caller (lowercase), stamped | **MISSED** | none (LOW-1) |
| B4 `Get-Process -Id $fetchPid \| ForEach-Object Kill`, stamped | **MISSED** | none (LOW-2) |
| B5 `[Diagnostics.Process]::GetProcessById($fetchPid).Kill()`, stamped | CAUGHT | K5, K6 |
| B6 `$script:gitExe = 'cmd.exe'`, stamped | **MISSED** | none (LOW-2) |
| B7 `Stop-Process` on a `#`-line inside a single-quoted here-string, then `iex`, stamped | **MISSED** | none (INFO-3, subsumed by LOW-2) |
| B8 `<# never Stop-Process here #>` block comment, stamped | CAUGHT | K7 (false positive, fail-safe) |
| B9 `$null = 1  # not taskkill` trailing comment, stamped | CAUGHT | K7 (false positive, fail-safe) |
| M2 rerun: dead `Stop-Process`, stamped | CAUGHT | K7 |
| M3 rerun: `Assert ($PvPinSha256 -eq $PvPinSha256)` | CAUGHT | K11 |
| M4 rerun: M3 plus dead `Stop-Process`, not stamped | CAUGHT | K7, K11 |
| M11 rerun: C# `OpenProcess` extern, stamped | CAUGHT | K10 |
| D1 real compare wrapped in `if ($false) { ... }`, plus a benign push_verify edit that is not stamped | **MISSED** | none (INFO-1) |
| D2 `$pvHash = $PvPinSha256` before the compare, plus a benign edit that is not stamped | **MISSED** | none (INFO-1) |
| D3 compare commented out (`# Assert ($pvHash -eq ...`), plus a benign edit that is not stamped | **MISSED** | none (INFO-1) |
| E1 benign edit, pin stamped, `# reviewed:` pointed at `GATE_NEGATIVE_TEST_EVIDENCE.md` with the hash appended | **MISSED** | none (INFO-2, by design) |
| E2 benign edit, pin bumped, doc not updated | CAUGHT | K13 |

All four TOOLFX15 MISSED mutations (M2, M3, M4, M11) are now CAUGHT on the intended asserts.

## Findings

### LOW-1: the PowerShell-side counts are case-sensitive, but PowerShell is not
`tools/check_push_verify.ps1:102` `Count-Re` uses `[regex]::Matches` with no options, so it is
case-sensitive. PowerShell resolves cmdlet names, type names, static and instance methods, and
variables case-insensitively. As a result, K1 (`add-type`), K2/K3/K4 (`[pvjob]::launch/kill/killproc(`),
K2b (`$GITEXE =`), K5 (`.kill()`), K6 (`getprocessbyid`) and K7 (`stop-process`, `TASKKILL`) can all be
satisfied while the forbidden call is present. B1 and B3 show this with a stamped pin. Commit
`8381e658a` claims "A bumped pin cannot satisfy these", and that claim is false as written.
Fix: in `Count-Re`, use `[regex]::Matches($s, $re, 'IgnoreCase')` for every count over `$pvSrc` and
`$pvLibSrc`. K8-K10 run over the C# text, which is case-sensitive, so they can stay as they are.
Making K10 case-insensitive is also harmless.

### LOW-2: K7 and K2b miss common alternative spellings
`tools/check_push_verify.ps1:111` K7 lists `Stop-Process|taskkill|Invoke-CimMethod|\.Terminate\(|WScript\.Shell`
and nothing else. It does not match:
- the built-in aliases `spps` and `kill` (B2);
- member-name invocation `ForEach-Object Kill` or `% Kill` (B4);
- `.Kill( )` with a space, which also gets past K5;
- `iex` / `Invoke-Expression` of a built string (B7).

`tools/check_push_verify.ps1:106` K2b matches only `$gitExe =`. It does not match `$script:gitExe =`
(B6), `${gitExe} =` or `Set-Variable gitExe`.

None of these lists can ever be complete. The pin plus a real review stays the control, and these
asserts are only a tripwire for a careless bump. A careless author is still likely to write `kill`
or `% Kill`. Cheap additions to K7 (case-insensitive per LOW-1):
- `(?<![\w:.\[-])(spps|kill)\b`, which matches the bare alias but not `[PvJob]::Kill(` or `.Kill(`;
- `(ForEach-Object|%)\s+(-MemberName\s+)?Kill\b`;
- `\.Kill\s*\(\s*\)` in K5;
- `\biex\b|Invoke-Expression`.

For K2b, use `\$(\{)?((script|global|local|private):)?gitExe\}?\s*=` and add a ban on
`(Set|New)-Variable\s+(-Name\s+)?gitExe`. Change the K7 message to "no known process-kill
spelling" so it does not promise completeness.

### INFO-1: K11 can pass while the real compare is dead
`tools/check_push_verify.ps1:115` counts the text `Assert ($pvHash -eq $PvPinSha256)` and accepts any
occurrence, including one that is:
- inside `if ($false) { }` (D1);
- in a comment (D3);
- placed after `$pvHash` has been reassigned to the pin (D2).

D3, commenting the assert out to "temporarily" get past it, is the realistic careless edit. All three
are edits to the check itself, and a reviewer of that diff would see them. TOOLFX15 INFO-1 already
noted this, so it stays INFO. A cheap fix stays AST-based on the check's own source:
- require exactly one `CommandAst` named `Assert` whose first argument is the `($pvHash -eq $PvPinSha256)`
  expression, and require that its `Parent.Parent` is the root `NamedBlockAst` (a top-level
  statement, not inside `if`, a function or a comment);
- require exactly one `AssignmentStatementAst` whose left side is `$pvHash`, and exactly one whose
  left side is `$PvPinSha256`.

### INFO-2: K13 links the bump to a document, but it does not prove a review happened
`tools/check_push_verify.ps1:116-119`. E1 shows that a bumper can point `# reviewed:` at any existing
audit doc and append the hash to it. The friction is real but small: the bumper has to make a
deliberate second edit, and the result leaves an audit trail that names a document. That trail is
the useful part. A check cannot prove that a review happened, and requiring a new file or a
same-commit doc change would be overengineering. Keep K13 as it is. An optional one-line
tightening would require that the hash appears on a line that also contains `Kill-surface pin
reviewed`, the phrase the TOOLFX15 doc uses, so the stamp has to be explicit.

### INFO-3: the comment handling in K7 is fail-safe in practice
`tools/check_push_verify.ps1:111` strips only full-line `#` comments (`(?m)^\s*#.*$`).
- Block comments (`<# #>`) and trailing `# ...` comments are not stripped. A harmless mention there
  gives a false positive (B8, B9 CAUGHT). This is the safe direction, and the current file has no
  such mention.
- A line that starts with `#` inside a here-string is stripped even though it is data (B7). It is
  only reachable through `iex`, which LOW-2 covers.
- The C# here-string has no `#` lines today, and K8-K10 run on `$pvCs` with `//` comments stripped,
  not `#`. So no real C# code is falsely stripped.

Nothing to fix beyond LOW-2.

### INFO-4: the TOOLFX15 fix status was not updated
`docs/audits/REVIEW_TOOLFX15_2026-10-12.md` "Fix status" still says "Open: LOW-1, LOW-2, LOW-3, INFO-1,
INFO-2". The correct status is:
- LOW-1: fixed in `8381e658a`, with the gaps in LOW-1/LOW-2 above;
- LOW-2: fixed in `872348b7c`;
- LOW-3: fixed in `8381e658a`;
- INFO-1: K11 added, see INFO-1 above;
- INFO-2: still open.

The `GATE_NEGATIVE_TEST_EVIDENCE.md` row for `check_push_verify.ps1` still cites only the 10-09
F7 mutation. Update both when LOW-1/LOW-2 are fixed.

## Fix status

LOW-1: FIXED (toolfx17: Count-Re is case-insensitive). LOW-2: FIXED (K7 gains spps/kill/iex/Invoke-Expression/ForEach-Object Kill/% Kill/.Terminate with spaces over comment-stripped code, K5 .Kill\s*(, K2b any scope prefix/braces plus Set-Variable ban; K7 message reworded as a tripwire). INFO-1: FIXED (K11 is an AST check: exactly one top-level pin Assert, K11b: one assignment each to $pvHash and $PvPinSha256). INFO-4: FIXED (TOOLFX15 Fix status and the GATE_NEGATIVE_TEST_EVIDENCE row updated). INFO-2 and INFO-3: no action recommended. B8/B9 (comments mentioning a kill word) now pass because K7 scans token-stripped code, not raw text.

Kill-surface pin reviewed: this fix changes only check_push_verify.ps1, not push_verify.ps1 or lib_push_verify_walk.ps1, so the pin is unchanged: `51b9bb56ed585d30347ba3e132610e4617f034b6872880a51e569861033c9869`.
