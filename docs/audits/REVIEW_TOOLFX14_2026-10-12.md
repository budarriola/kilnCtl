# Review: toolfx14 fix commits (a1db558a8, 55f173a18) -- 2026-10-12

This is an adversarial, read-only review of the two origin/dev commits that answer
`REVIEW_TOOLFX13_2026-10-12.md` MED-1, LOW-1, LOW-3 and LOW-4. It was done at `b69ba4a94`:

- `a1db558a8`: `Get-LintCount`, nine kill-lint bypasses closed, `check_land.ps1` attempt-count and
  re-check counting asserts, a forged `remote:` verdict case, and the land doc fixes.
- `55f173a18`: two more hole cases: the ForEach-Object member-name string form and the pinned
  `Launch` first argument.

No reviewed code was changed by this review.

Evidence sources:

- `Get-KillLintViolations`/`Get-LintCount`, extracted from `check_push_verify.ps1` and run directly
  on `push_verify.ps1` with candidate payloads appended or edited in.
- Semantic proof on sacrificial `ping -n 60` processes that this review started itself. No other
  process was touched.
- Two `tools\negtest.ps1 -Preset check` runs with `-Mutations` JSON and
  `-ExpectPattern '(?m)^\s+FAIL: '`. Both baselines passed. Both runs ended with
  `real_tree_unchanged: true` and `copies_removed: true`, and no `REAL TREE CHANGED` line.

## Verdict

No HIGH and no MED. The four fixes do what they claim:

- **MED-1 is fixed.** `Get-LintCount` (`tools/check_push_verify.ps1:87`) assigns before counting,
  so the count is real. Mutant `lintcount_vacuous` restores the old `@(...).Count` form and is
  CAUGHT by the clean-source asserts at lines 179-180. Mutant `lint_drop_cmd_from_bad` was MISSED
  in TOOLFX13 and is now CAUGHT ("L1 hole closed (script): cmd taskkill").
- **LOW-3 is fixed.** The three land.ps1 mutants that TOOLFX13 reported as MISSED are now CAUGHT.
  Each CAUGHT comes from a real `Assert` failure, not a crash (see the table).
- **LOW-4 is fixed.** The text is correct at `docs/MCP_SERVERS.md:753`, around `:880-886`, and in
  the `tools/land.ps1:56-58` header.
- **LOW-1 is fixed only for the nine forms that were listed.** The lint is still a deny-list. Its
  two pinned sites are pinned by variable name, not by value. Both can be bypassed with a one-line
  edit, and several other kill or spawn paths still pass with 0 violations (LOW-1 and LOW-2 below).

## Findings

### LOW-1: the two pinned sites are pinned by name only; a one-line edit turns them into a kill

Locations: `tools/check_push_verify.ps1:131`, `:108` and `:143-147`.

**`[PvJob]::Launch`.** The lint requires exactly one `[PvJob]::Launch` whose first argument text
is `$gitExe`. It never checks what is assigned to `$gitExe` or `$fetchCmd`. Inserting this line
before the call at `tools/push_verify.ps1:207` gives **0 violations**:

```
$gitExe = "$env:SystemRoot\System32\taskkill.exe"; $fetchCmd = "taskkill /F /PID 1"
```

`CreateProcessW(gitExe, cmdLine)` then runs taskkill.

**`Add-Type`.** The lint requires exactly one `Add-Type`, but does not pin its argument to the one
here-string. Changing `-TypeDefinition @"` to
`-TypeDefinition ('public class Zk { public static void K(int p){ System.Diagnostics.Process.GetProcessById(p).Kill(); } }' + @"`,
and adding `[Zk]::K($fetchPid)`, gives **0 violations**. The reasons:

- the forbidden C# API scan (line 161) reads only `DoubleQuotedHereString` constants;
- the constant-string scan (line 147) looks for `DllImport|OpenProcess|NtTerminate|TerminateProcess|TerminateJobObject`
  only, not `GetProcessById` or `.Kill(`;
- `[Zk]` is not a denied type.

Fix options:

- Pin values, not just names: require exactly one assignment to `$gitExe` (the
  `Get-Command git -CommandType Application` form), exactly one to `$fetchCmd`, and an `Add-Type`
  whose only argument is the one here-string.
- Or apply the forbidden-API regex to every string constant.
- Simpler and stronger: pin the SHA-256 of `push_verify.ps1`'s kill surface (the `Add-Type` block
  and the `try`/`finally` fetch block) in the check. Any edit then needs a deliberate re-pin that a
  reviewer sees.

LOW for the same reason as TOOLFX13 LOW-1: every bypass needs an edit to `push_verify.ps1`.

### LOW-2: leaving New-Object allowed opens a spawn path; other deny-list gaps remain

Each payload below was appended to `push_verify.ps1` as `if ($false) { ... }`, and
`Get-KillLintViolations` reported **0 violations**:

| Payload | Effect | Semantically proven here |
|---|---|---|
| `(New-Object -ComObject WScript.Shell).Run("taskkill /F /PID $fetchPid", 0)` | spawn / kill | yes, killed a sacrificial ping |
| `(New-Object -ComObject Shell.Application).ShellExecute("taskkill.exe", ...)` | spawn | no (same class) |
| `(New-Object System.Management.ManagementObjectSearcher("...")).Get().ForEach('Terminate')` | kill | via B1 |
| B1 `([wmisearcher]"select * from Win32_Process where ProcessId=$fetchPid").Get().ForEach('Terminate')` | kill | yes |
| B2 `$mm='Terminate'; ... \| ForEach-Object $mm` | kill | yes |
| B3 `$pp=@{MemberName='Terminate'}; ... \| ForEach-Object @pp` | kill | yes |
| B4 `... \| ForEach-Object "Term$('inate')"` (expandable string) | kill | no (same binding as B2) |
| `tskill $fetchPid`; `& "C:\Windows\System32\tskill.exe" $fetchPid` | kill | no |
| `& "C:\somewhere\kill.ps1"`; `Import-Module C:\x\evil.psm1` | arbitrary code | no |
| `$sb = $ExecutionContext.InvokeCommand.NewScriptBlock('Stop-' + 'Process -Id 1'); 1 \| Where-Object $sb` | kill | no |
| `Restart-Computer -Force`; `schtasks /Run ...`; `Debug-Process -Id 1` | host-wide | no |

Root causes:

- The type rule (line 116) checks only `TypeExpressionAst` (`[X]::m`). A cast such as
  `[wmisearcher]"..."` or `[wmi]"..."` is a `ConvertExpressionAst`, so WMI is reachable without
  `Get-WmiObject`.
- The member rule (line 135) does not list the intrinsic `.ForEach`/`.Where` (string form), `Run`,
  `ShellExecute`, `Exec` or `NewScriptBlock`.
- The ForEach-Object rule (lines 109-114) flags only a literal `-M*` parameter or a
  `StringConstantExpressionAst`. It misses a variable, a splat and an expandable string.
- External command names are deny-listed, not allow-listed, so any executable or script not on the
  list passes.

**Judgement on `Get-CimInstance`:** no direct hole was found. CIM instances carry no methods, and
`Invoke-CimMethod`, `Remove-CimInstance` and `.InvokeMethod` are all denied. Keeping it allowed is
fine; `push_verify.ps1` needs it for the snapshot walk.

**Judgement on `New-Object`:** it is a hole, through `-ComObject` (WScript.Shell, Shell.Application)
and through `-TypeName System.Management.*`. The type regex at line 118 never sees a type passed to
`New-Object` as a string. `push_verify.ps1` uses `New-Object` only for
`System.Collections.ArrayList` (line 220). Fix: allow only `New-Object System.Collections.ArrayList`
(or replace it with `[System.Collections.ArrayList]::new()`), and deny `-ComObject` outright.

The real fix is the one TOOLFX13 recommended and this round did not adopt: a true allowlist of the
command names, types and member names the two files actually use. Alternatively, use the hash pin
from LOW-1.

### LOW-3: three of the new lint rules have no test that fails without them

`check_push_verify.ps1` negtest (table below). These three mutants were MISSED:

- `drop_exe_strip`: removes `-replace '(?i)\.(exe|com|bat|cmd)$', ''` (line 99). The absolute-path
  taskkill hole is still caught, because `$bad` already allows `taskkill(\.exe)?`. No hole case uses
  an extension that `$bad` lacks, such as `stop-process.ps1`-style or `Stop-Process.cmd`, or
  `tskill.exe` once tskill is denied.
- `drop_string_const_scan`: disables the constant-string scan (line 147). Both hole cases it was
  written for (`second single-quoted Add-Type`, `concatenated EntryPoint`) are also caught by the
  `Add-Type` count, so the scan itself is never exercised.
- `drop_cs_entrypoint`: removes `EntryPoint` from the here-string forbidden-API regex (line 161).
  The `concatenated EntryPoint` hole puts the C# in a single-quoted string, which this regex never
  reads.

Fix: add one hole case per rule that only that rule catches. Examples:

- a `.Replace()` on `$pvSrc` that puts `EntryPoint = "Open" + "Process"` inside the real here-string;
- a string constant `'DllImport'` with no second `Add-Type`;
- `C:\x\Stop-Process.cmd`.

### INFO

- INFO-1: `tools/land.ps1:403`: `}    Finish 1 "git push failed (not a non-fast-forward rejection)..."`.
  The message predates the race classifier; this path is now the `other` verdict. The same-line
  `}` is harmless (TOOLFX13 INFO-7). Suggest the text "not a race and not a recognised refusal".
- INFO-2: `tools/check_land.ps1:135`: the assert text says the checks re-ran "against the newly
  rebased tree", but it counts only stub invocations (2). The mutant it targets
  (`recheck_only_first_attempt`) is caught, which is what matters. The tree identity is not
  asserted.
- INFO-3: `$fetchCmd` (`tools/push_verify.ps1:197`) is a git command line, so a
  `-c alias.x=!<shell>` edit would spawn a shell through the one legitimate `Launch`. This is
  inherent to running git, and the hash pin in LOW-1 is the only lint-level answer.
- INFO-4: several hole cases are caught by more than one rule. For example, `WMI ForEach-Object
  Terminate` hits both `gwmi` and the ForEach string rule. So a passing hole list does not prove
  each rule is live. LOW-3 is the measured consequence.
- INFO-5: `55f173a18` carries a `Claude Sonnet 5.5` co-author trailer, while `a1db558a8` carries
  `Claude Opus 5.5`. Cosmetic.
- INFO-6: the fixer's interrupted `check_land.ps1` negtest ("REAL TREE CHANGED") was re-run here
  cleanly. All three mutants were CAUGHT by real assert failures, with `real_tree_unchanged: true`.

## Mutation evidence

`tools\negtest.ps1 -Preset check -PresetArg tools/check_land.ps1 -Parallel 3 -ExpectPattern '(?m)^\s+FAIL: '`
(baseline passed in 482 s; ALL_CAUGHT; `real_tree_unchanged: true`, `copies_removed: true`)

| Mutation | Verdict | Assert evidence (each log: exactly 1 FAIL, `check_land: 1 FAILED`, no parse error) |
|---|---|---|
| `verdict_drop_caret` (drop `^` on the `!<TAB>` regex, `land.ps1:105`) | CAUGHT | "verdict forged remote:-prefixed status line before a real decline -> refused" |
| `maxtries_default_5` (`MaxPushTries` 3 -> 5, `land.ps1:80`) | CAUGHT | "LOW-3: a persistent race stops after exactly 3 attempts (rebases: 5)" |
| `recheck_only_first_attempt` (checks run only when `$try -eq 1`, `land.ps1:370`) | CAUGHT | "LOW-3: the post-rebase narrow checks re-ran on the retry ... (stub runs: 1, want 2)" |

`tools\negtest.ps1 -Preset check -PresetArg tools/check_push_verify.ps1 -Parallel 4 -ExpectPattern '(?m)^\s+FAIL: '`
(baseline passed in 161 s; 7 CAUGHT, 3 MISSED; `real_tree_unchanged: true`, `copies_removed: true`)

| Mutation | Verdict | Assert evidence |
|---|---|---|
| `lint_drop_cmd_from_bad` (TOOLFX13's MED-1 proof) | CAUGHT | "L1 hole closed (script): cmd taskkill" |
| `lintcount_vacuous` (`@(Get-KillLintViolations ...).Count`) | CAUGHT | "L1: Get-LintCount returns 0 for the clean sources", "L1: a harmless extra command is not flagged" |
| `drop_path_strip` (line 99 removed) | CAUGHT | "module-qualified Stop-Process", "absolute-path taskkill" |
| `drop_exe_strip` | MISSED | no unique hole case (LOW-3) |
| `drop_foreach_string` (line 112 -> `elseif ($false)`) | CAUGHT | "ForEach-Object member-name string" |
| `drop_launch_pin` (line 131 -> `if ($true)`) | CAUGHT | "the one Launch retargeted at another exe" |
| `drop_addtype_count` (line 143 -> `if ($false)`) | CAUGHT | "second single-quoted Add-Type" |
| `drop_string_const_scan` (line 147 -> `if ($false)`) | MISSED | shadowed by the Add-Type count (LOW-3) |
| `drop_cs_entrypoint` (`EntryPoint` out of line 161) | MISSED | hole case never reaches the here-string scan (LOW-3) |
| `drop_invoke_wildcard` (`Invoke\w*` -> `Invoke`) | CAUGHT | "InvokeScript" |

Every CAUGHT log was inspected. Each has one or two real `  FAIL: <assert text>` lines, a final
`check_*: N FAILED` line, and no parse error. Every MISSED log ends with
`check_push_verify: all cases passed`.

## Fix status (toolfx15)

LOW-1, LOW-2, LOW-3 and the land.ps1 INFO text are FIXED. The deny-list in 	ools/check_push_verify.ps1 was removed and replaced by a SHA-256 pin over push_verify.ps1 + lib_push_verify_walk.ps1 (any edit fails the check until a reviewer updates PvPinSha256). The vacuous deny-list rules (.exe strip, string-constant scan, C# EntryPoint) went with it. Negtest (-Preset check): reassign gitExe before Launch, GetProcessById(p).Kill() in the Add-Type, and WScript.Shell .Run were each CAUGHT by the pin's FAIL line. 	ools/land.ps1 push-failure text no longer claims a non-fast-forward rejection.
