# Review: toolfx17 (check_push_verify fixes for REVIEW_TOOLFX16), 2026-10-12

Scope: origin/dev `d61735c6e` (`tools/check_push_verify.ps1` fixes for `REVIEW_TOOLFX16_2026-10-12.md`
LOW-1, LOW-2, INFO-1 and INFO-4) and `3d05b7a19` (status updates in REVIEW_TOOLFX15/16 and the
`GATE_NEGATIVE_TEST_EVIDENCE.md` row). The changes:

- `Count-Re` is now case-insensitive.
- K7 now scans `$pvPs`, which is `push_verify.ps1` with Comment and HereStringExpandable tokens removed.
- K5 and K2b were widened.
- K11 is now AST-based, and a new K11b counts assignments to `$pvHash` and `$PvPinSha256`.

Reviewed in a fresh worktree at `3d05b7a19`. The pin (`51b9bb56...`) and `push_verify.ps1` are
unchanged by these commits.

**Severity counts: 0 HIGH, 0 MED, 4 LOW, 4 INFO.**

None of these findings lets a kill in `push_verify.ps1` land without a human stamping the pin. Every
MISSED stamped mutant still requires changing `$PvPinSha256` and appending the new hash to the reviewed
doc, which a reviewer sees in the diff. The pin-integrity gaps (LOW-3, LOW-4, INFO-1) are different:
they let an edit to `check_push_verify.ps1` itself neuter the pin while K11 and K11b still pass.

## Evidence

- `check_push_verify.ps1` run directly in the worktree passes (negtest baseline: rc 0, 76.8 s).
- I ran `tools\negtest.ps1 -Preset check -PresetArg tools\check_push_verify.ps1 -Mutations <json> -ExpectPattern "FAIL: "`
  with 30 mutations. The baseline passed, `real_tree_unchanged` was true and `copies_removed` was
  true. The mutation JSON and its generator are kept in a private scratch directory, not in the repo.
- **Stamped mutants (N\*, R-B\*):** each edits `push_verify.ps1`. The pin line is set to the
  mutant's surface hash, computed offline, and that hash is appended to the reviewed doc
  (`REVIEW_TOOLFX15_2026-10-12.md`). Every injected kill is inside `if ($false)`. In each log, L1,
  K11, K11b and K13 read `ok`, so the stamp was correct and the mutant parsed.
- **Check-self mutants (D\*):** each makes a benign `push_verify.ps1` edit that is *not* stamped, so
  the pin is stale, and edits `check_push_verify.ps1` to try to neuter the pin compare. D5, D6 and D7
  logs show `L1 ... match the pinned kill-surface hash: ok`, while the printed surface sha256 is
  `9ada72d6...`, not the pin `51b9bb56...`.
- Every CAUGHT mutation fails on exactly one named `  FAIL: K<n>` line, which I checked by grepping
  every log. No mutation was caught by a parse error. Every MISSED mutation ends `all cases passed`
  with exit 0.

| # | Mutation | Verdict | Failing assertion |
|---|---|---|---|
| 1 | N1 `Stop-Process` inside `$()` of a second expandable here-string | **MISSED** | none (LOW-1) |
| 2 | N2 `& ('Stop'+'-Process')` | **MISSED** | none (INFO-2) |
| 3 | N3 `& 'Stop-Process'` | CAUGHT | K7 |
| 4 | N4 `& (Get-Command Stop-P*)` | **MISSED** | none (INFO-2) |
| 5 | N5 `$ExecutionContext.InvokeCommand.InvokeScript(<concat>)` | **MISSED** | none (INFO-2) |
| 6 | N6 `[scriptblock]::Create(@"...Stop-Process..."@)` | **MISSED** | none (LOW-1) |
| 7 | N6b `[scriptblock]::Create(@'...Stop-Process...'@)` | CAUGHT | K7 |
| 8 | N7 `Start-Process taskkill` | CAUGHT | K7 |
| 9 | N8 `cmd /c tskill` | **MISSED** | none (INFO-2) |
| 10 | N8b `cmd /c wmic process ... delete` | **MISSED** | none (INFO-2) |
| 11 | N9 `Get-CimInstance Win32_Process \| Remove-CimInstance` | **MISSED** | none (INFO-2) |
| 12 | N10 `Invoke-WmiMethod -Name Terminate` | **MISSED** | none (INFO-2) |
| 13 | N10b `Get-WmiObject ... \| % Terminate` | **MISSED** | none (INFO-2) |
| 14 | N11 type-accelerator alias `[PvP]` plus `.Kill.Invoke()` | **MISSED** | none (INFO-2) |
| 15 | N11b `(Get-Process -Id $p).Kill.Invoke()` | **MISSED** | none (INFO-2) |
| 16 | N12 `.PSObject.Methods['Kill'].Invoke()` | CAUGHT | K7 |
| 17 | N13 `.PSObject.Methods[('Ki'+'ll')].Invoke()` | **MISSED** | none (INFO-2) |
| 18 | N16 `sv gitExe $env:ComSpec` | **MISSED** | none (INFO-3) |
| 19 | N17 control: `Stop-Process` | CAUGHT | K7 |
| 20 | R-B2 rerun of TOOLFX16 B2: `spps` | CAUGHT | K7 |
| 21 | R-B4 rerun of TOOLFX16 B4: `ForEach-Object Kill` | CAUGHT | K7 |
| 22 | R-B6 rerun of TOOLFX16 B6: `$script:gitExe = ...` | CAUGHT | K2b |
| 23 | N14 C# `Process.Start("taskkill", ...)` in the Add-Type body, plus a caller | **MISSED** | none (LOW-2) |
| 24 | N15 C# `x.Kill()` in the Add-Type body, plus a caller | CAUGHT | K5 |
| 25 | D4 `function Assert { }` shadow defined before the pin compare, real Assert restored after | **MISSED** | none (INFO-1) |
| 26 | D5 pin compare moved into a function that is never called | **MISSED** | none (LOW-3) |
| 27 | D6 pin compare moved into a scriptblock that is never invoked | **MISSED** | none (LOW-3) |
| 28 | D7 `$script:pvHash = $PvPinSha256` before the compare | **MISSED** | none (LOW-4) |
| 29 | R-D3 rerun of TOOLFX16 D3: compare commented out | CAUGHT | K11 |
| 30 | R-D1 rerun of TOOLFX16 D1: compare under `if ($false)` | CAUGHT | K11 |

All the TOOLFX16 mutations that were MISSED and are rerun here (B2, B4, B6, D1 and D3) are now CAUGHT
on the intended assertions. The B1 and B3 class (lowercase spellings) is covered by the
case-insensitive `Count-Re`, as the R-B rows confirm.

## Findings

### LOW-1: the K7 strip removes every expandable here-string, including executable code (regression)
`tools/check_push_verify.ps1:107` removes every `HereStringExpandable` token from `$pvPs` before K7
runs. The intent (line 104) was to drop only the C# `Add-Type` literal, which K8-K10 cover. But an
expandable here-string is not inert:

- Any `$( ... )` inside it is a subexpression that executes. N1 puts `Stop-Process` inside `$()` in a
  second `@"..."@` and passes.
- Any expandable here-string can be fed to `[scriptblock]::Create` or `iex`. N6 passes; the
  single-quoted twin N6b is still caught.

Neither string reaches K8-K10. `$pvCs` (line 99) collects only `StringConstantExpressionAst` nodes,
and a here-string containing `$()` parses as an `ExpandableStringExpressionAst`. The text is
therefore scanned by nothing. Before toolfx17, K7 scanned the raw text minus full-line comments, so it
caught N1 and N6.

Fix: strip only the one C# literal. Identify it as the here-string argument of the single
`Add-Type -TypeDefinition` command (by AST parent or by extent offset), and keep every other
here-string in `$pvPs`. Optionally also assert that `push_verify.ps1` contains exactly one
double-quoted here-string.

### LOW-2: the C# body is no longer scanned for process-launch or kill spellings (regression)
Because of the same strip, the Add-Type C# is now checked only by K5 (`.Kill(`), K8, K9 and K10
(`OpenProcess|GetProcessById|DuplicateHandle|EntryPoint`). N14 adds
`System.Diagnostics.Process.Start("taskkill", "/F /PID " + pid)` to the C# class and a PowerShell
caller, and passes. Before toolfx17, K7 ran over the raw text, C# included, and would have caught
`taskkill`. N15 (`x.Kill()` in C#) is still caught by K5.

Fix: add a K10b over `$pvCs` for
`taskkill|tskill|wmic|Process\.Start|cmd\.exe|GetProcesses|TerminateProcess|Terminate\s*\(`, using the
case-insensitive `Count-Re`. Keep the existing `CreateProcessW(gitExe,` site as the only process launch
allowed.

### LOW-3: K11 accepts a pin Assert inside a function or an uninvoked scriptblock
K11 (line 121) checks only that the Assert's `Parent.Parent` is a `NamedBlockAst` and its
`Parent.Parent.Parent` is a `ScriptBlockAst`. Every function body and every `{ ... }` literal has
that same shape. Moving the compare into a function that is never called (D5), or into a scriptblock
that is never invoked (D6), keeps K11 and K11b green. The pin is then never compared, so a stale pin
passes L1, which is exactly what K11's own message says it prevents ("not under if/function").

Fix: require `$pvAsserts[0].Parent.Parent.Parent -eq $pvSelfAst`, which is reference equality to the
file's root `ScriptBlockAst`. Also consider requiring that the Assert's statement comes after both
assignments, using extent offsets.

### LOW-4: K11b misses scope-qualified assignments to `$pvHash`
`Count-Assign` (line 122) compares `VariablePath.UserPath`, which keeps the scope prefix. So
`$script:pvHash = $PvPinSha256` (D7) counts as an assignment to `script:pvHash`, not `pvHash`. K11b
still sees one assignment to each variable, and the compare becomes `$PvPinSha256 -eq $PvPinSha256`
at run time. At script scope, `$script:pvHash` and `$pvHash` are the same variable. K2b already
handles scope prefixes for `$gitExe` (R-B6 is caught).

Fix: compare `VariablePath.UnqualifiedPath` (case-insensitive) in `Count-Assign`. Also count
`Set-Variable`, `sv`, `New-Variable` and `variable:` writes that name either variable, as K2b intends
to.

### INFO-1: Assert can be shadowed around the pin compare
D4 defines `function Assert { param($c, $m) }` just before the pin compare and restores the real
Assert afterward. K11 sees one `Assert` CommandAst with the right arguments in the right place.
The compare runs but cannot fail, and L1 prints `ok` for a stale pin. An alias would do the same,
since an alias takes precedence over a function. This is a check-self edit, so it shows up in
review, but it neuters the control the pin depends on.

Fix: require exactly one `FunctionDefinitionAst` named `Assert` (case-insensitive) in
`check_push_verify.ps1`, positioned before the pin compare. Require no `Set-Alias`, `New-Alias`, `sal`
or `nal` and no `function:`/`alias:` provider writes anywhere in the file.

### INFO-2: tripwire bypasses (expected for a deny-list)
K7 is a deny-list. Its own message says it is "a tripwire for a careless edit, not a proof; the pin
plus review is the control". These stamped mutants pass:

- **Dynamic invocation:** N2, N4, N5 and N13.
- **External kill tools:** N8 (`tskill`) and N8b (`wmic ... delete`).
- **CIM/WMI:** N9 (`Remove-CimInstance`), N10 (`Invoke-WmiMethod -Name Terminate`) and N10b
  (`% Terminate`).
- **Method-object invocation:** N11 and N11b (`.Kill.Invoke()`).

Cheap additions that would catch the literal spellings: `tskill`, `wmic`, `Remove-CimInstance`,
`Invoke-WmiMethod`, `(ForEach-Object|%)\s+(-MemberName\s+)?Terminate\b`, and `\.Kill\b` without
requiring `(`. The dynamic forms cannot be closed by a regex. The pin is the control for those.

### INFO-3: K2b counts assignments to `$gitExe` but does not check the source, and misses `sv`
N16 (`sv gitExe $env:ComSpec`) passes. K2b's message says "no Set-Variable", but the `sv` alias
(and `si variable:gitExe`) is not matched. K2b also does not check that the single assignment's
right-hand side is the `Get-Command git -CommandType Application` resolution. Fix: match
`\b(Set-Variable|sv|New-Variable|nv|Set-Item|si)\b[^\n]*gitExe` and `variable:gitExe`, and assert the
right-hand side of the one assignment AST.

### INFO-4: the INFO-3 assertion can be satisfied by a comment
Line 129 matches `Get-Command git -CommandType Application` against the raw `$pvSrc`, so a comment
containing that text satisfies it even if the real resolution is removed. Fix: match against
`$pvPs`, once LOW-1 is fixed so `$pvPs` keeps the PowerShell code, or check the `$gitExe` assignment
AST as in INFO-3.

## Fix status

Open: LOW-1, LOW-2, LOW-3, LOW-4 and INFO-1 through INFO-4 are pooled for a fixer. After the fix,
rerun the 30 mutations above. Every row should be CAUGHT on a named `FAIL: K<n>` line, except
INFO-2's dynamic forms (N2, N4, N5, N13), which are accepted as residual risk under the pin.
