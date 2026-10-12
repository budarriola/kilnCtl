# Review: toolfx13 fix commits (aa7cc80e1, b369df678, 99f81431b) -- 2026-10-12

This is an adversarial, read-only review of the three origin/dev commits that answer
`REVIEW_TOOLFX12_2026-10-12.md`. It was done at `99f81431b`:

- `aa7cc80e1`: `land.ps1` push-race classifier (`Get-PushRejectVerdict`), kill-surface lint in
  `check_push_verify.ps1`, root-kill and test-hook tests.
- `b369df678`: the LOW-2 test uses the mingw64 `git.exe`.
- `99f81431b`: REVIEW_TOOLFX12 fix-status table.

No reviewed code was changed by this review.

Evidence sources:

- Live `git push --porcelain` captures, using Git for Windows 2.52.0 against a throwaway bare
  origin in a scratch directory (never the real origin).
- The extracted `Get-PushRejectVerdict` function, run directly on real and synthetic outputs.
- The extracted `Get-KillLintViolations` function, run directly on mutated sources.
- Two `tools\negtest.ps1 -Preset check` runs with `-Mutations` JSON and
  `-ExpectPattern '(?m)^\s+FAIL: '`, so a CAUGHT requires a real `Assert` failure line. Both
  baselines passed and both runs ended with `real_tree_unchanged: true`.

## Verdict

No HIGH. One MED: every kill-lint self-test that the fix-status table cites for LOW-3 is
vacuous. The classifier is sound for git 2.52 and for GitHub. It retried no genuine refusal I
could construct, and its only wrong answer refuses a race (fail closed). The retry loop is bounded
and re-runs fetch, rebase, the narrow checks and the pin check on every attempt. No regression found
on the `-Target main`, `-CheckLog`, `-AllowKnownFailures` or worktree-removal paths. None of them is
touched by the diff.

## Findings

### MED-1: the kill-lint self-tests are vacuous; a weakened lint passes

Location: `tools/check_push_verify.ps1:175-180`.

`Get-KillLintViolations` ends with `return ,$v`. The unary comma wraps the ArrayList, so
`@(Get-KillLintViolations ...)` is always a one-element array. `.Count -gt 0` is then always true,
even for a clean source or `Write-Host hi`. The following "hole closed" asserts can therefore never
fail:

- the nine `$holes` script cases;
- the lib `Stop-Process` case;
- the lib `Kill` case;
- the extra C# `TerminateProcess` case;
- the C# `OpenProcess` case.

The one real assertion is the clean-source check at the top, which assigns the result before
counting. It proves only that the current sources are clean, not that the lint rejects anything.

Mutation evidence: `lint_drop_cmd_from_bad` removes `cmd(\.exe)?` from `$bad`. Result: MISSED,
exit 0, and the log shows `ok: L1 hole closed (script): ...` for every hole. The fix-status row
"in-test hole cases for every demonstrated hole" is therefore not backed by a working test.

Fix: assign before counting (`$x = Get-KillLintViolations ...; $x.Count -gt 0`), or drop the comma.
Then negative-test by removing an entry from `$bad`.

### LOW-1: the kill lint is a deny-list on bare names and has verified bypasses

Command names are matched with an anchored regex against `GetCommandName()`. Member names are
matched against a fixed verb set. Each of the following was appended as dead code
(`if ($false) { ... }`) to `push_verify.ps1`, and `Get-KillLintViolations` reported **0**
violations:

- `Microsoft.PowerShell.Management\Stop-Process -Id 1` (module-qualified name).
- `C:\Windows\System32\taskkill.exe /F /PID 1` (absolute path).
- `Set-Alias zz Stop-Process; zz` (alias created at runtime).
- `Get-WmiObject Win32_Process ... | ForEach-Object Terminate`. `Get-WmiObject` and
  `Get-CimInstance` are not denied, and `ForEach-Object <member>` invokes the method without an
  InvokeMemberExpressionAst.
- `$ExecutionContext.InvokeCommand.InvokeScript(...)` (the member regex matches only `Invoke`
  exactly).
- `[PvJob]::Launch("C:\Windows\System32\taskkill.exe", ...)`. `Launch` arguments are not pinned.
- A second `Add-Type` with single-quoted C# plus `[Zk]::K($fetchPid)`. Only double-quoted here-string
  C# is scanned, and the number of `Add-Type` calls is not limited.
- Module-qualified `Get-Process | ForEach-Object Kill`.
- C# `EntryPoint = "Open" + "Process"` (concatenated entry point).

negtest confirms three of them end to end: `bypass_module_qualified`,
`bypass_single_quoted_addtype` and `bypass_wmi_foreach_terminate` are all MISSED. The control
`control_plain_stop_process` is CAUGHT, with `lint: forbidden command: Stop-Process` and the real
`S2/L1` assert failure.

Every bypass needs a deliberate or careless edit to `push_verify.ps1`, so this is LOW. Fix:

- Make it a true allowlist of the command names the two files actually use.
- Strip any module qualifier and path, and drop `.exe`, before matching.
- Deny `Set-Alias`/`New-Alias`, `Get-WmiObject`/`Get-CimInstance`, `ForEach-Object -MemberName`,
  and any member whose name contains `Invoke`.
- Pin `Launch` arguments and allow exactly one `Add-Type`, scanning every string argument passed to
  it.

### LOW-2: a race reported by an older or non-GitHub git server is refused as a hook decline

On older git servers, a lost compare-and-swap reports the status reason `failed to update ref`. The
`cannot lock ref '<ref>': is at <40hex> but expected <40hex>` detail appears only on a separate
`remote: error:` line.

`Get-PushRejectVerdict` returns `refused` for this case. `land.ps1` then says "refused by the
remote (hook / protected branch), not a race", which is misleading. It fails closed and the agent
re-runs land, so this is LOW.

A safe refinement: treat it as a race when the status reason is exactly `failed to update ref` AND a
`remote: error: cannot lock ref '<same ref>': is at <40hex> but expected <40hex>` line is present.
A hook cannot produce the `failed to update ref` status reason; that string is receive-pack's own.
The servers this repo pushes to (git 2.52 locally, GitHub) use the two forms that are already
handled.

### LOW-3: test gaps around the retry loop and the status-line anchor

Three land.ps1 mutants survive `check_land.ps1`:

- `verdict_drop_caret`: drops the `^` anchor on the `!<TAB>` status-line regex. Result: MISSED. The
  mutant is not equivalent. Run directly on
  `remote: !<TAB>HEAD:refs/heads/dev<TAB>[rejected] (fetch first)` followed by a real
  `[remote rejected] (pre-receive hook declined)` line:
  - the original returns `refused`;
  - the mutant returns `race`, so hook prose could buy a retry.

  The fix-status claim of "unanchored-status-line cases in the verdict table" does not cover a
  forged line prefixed with `remote: `.
- `maxtries_default_5` (`MaxPushTries` 3 -> 5): MISSED. The persistent-race case asserts only the
  message, never the attempt count. It should assert that exactly 3 rebases ran.
- `recheck_only_first_attempt`: the post-rebase checks run only on attempt 1. Result: MISSED. No case
  proves that a retry re-runs the narrow checks against the newly rebased tree. A counting
  `-ChecksScript` stub in the race case would cover it.

### LOW-4: documentation now contradicts the code

- `docs/MCP_SERVERS.md:753` says "a `[remote rejected]` push is not retried". The two race forms of
  `[remote rejected]` are now retried.
- The `land.ps1` header, lines 55-57, says the push is "retried only on a non-fast-forward
  rejection". Ref-lock races are retried too.
- `docs/MCP_SERVERS.md` around lines 874-897 still describes `git rebase origin/main` and
  `git push origin HEAD:main` as the landing sequence. That predates the dev flow, where land
  defaults to `-Target dev`.

### INFO

- INFO-1: a held ref lock (`[remote rejected] (reference already exists)`, captured live) is
  classified `refused`. It can be a transient concurrent push, so this fails closed. Acceptable.
- INFO-2: on a SHA-256 repository the race regex (`[0-9a-f]{40}`) does not match 64-hex ids, so the
  race is refused. Fails closed. The repo is SHA-1.
- INFO-3: the first race status line wins over a later refusal line in the same output. Forging a
  raw `!<TAB>` line needs output that is not sideband-prefixed (`remote: `), which a hook cannot
  produce. No practical exposure.
- INFO-4: a `proc-receive` hook (AGit flow) can set an arbitrary status reason, including
  `incorrect old value provided`. That needs control of the server; out of scope.
- INFO-5: `-MaxPushTries 0` or a negative value skips the loop and finishes with "still rejected
  after 0 attempts". Misleading text, but nothing is pushed.
- INFO-6: if the rebase drops every agent commit as already upstream, HEAD equals the origin tip and
  `git push` exits 0 ("Everything up-to-date"). land then reports landed for a sha that is not the
  agent's. The content is on the branch, so it is benign. Found by reading the code; not
  demonstrated. The ahead-count guard at line 225 runs only before the loop.
- INFO-7: `check_push_verify.ps1:237`: `Write-Host "case: failed fetch ..."` sits on the same line as
  the preceding `Assert`, so it becomes an extra argument to `Assert` and is never printed. Only the
  header text is lost; the case still runs. Line 260 has a harmless same-line comment, and
  `land.ps1:403` a harmless same-line `}    Finish` (parsed and verified as a separate statement).
- INFO-8: `land_no_lc_all` (remove `LC_ALL=C`/`LANG=C`) is MISSED again. It is equivalent on this
  host: Git for Windows 2.52.0 ships no `git.mo` translations, and no other git build is installed.
  The `--porcelain` status lines are not translated anyway, only the free text is.

## Answers to the review questions

1. **Does the classifier ever retry a genuine refusal or refuse a genuine race?**

   Real git 2.52 porcelain, captured live:

   | Situation | Status line | Exit |
   |---|---|---|
   | Non-fast-forward | `!<TAB>HEAD:refs/heads/dev<TAB>[rejected] (fetch first)` | 1 |
   | Held ref lock | `[remote rejected] (reference already exists)` | 1 |
   | Pre-receive decline | `[remote rejected] (pre-receive hook declined)`, with the hook's prose prefixed `remote: ` | 1 |
   | Update hook | `[remote rejected] (hook declined)` | 1 |
   | `denyDeletes` | `[remote rejected] (deletion prohibited)` | 1 |

   Verdicts:

   - **race**: `fetch first`, `stale info`, the GitHub `cannot lock ref ...: is at <40hex> but
     expected <40hex>` reason, CRLF line endings.
   - **refused**: GH006 protected branch, GH013 rule violations, hook decline, held lock, atomic
     transaction failure, and a forged `remote: !<TAB>...` line followed by a real decline.

   No genuine refusal is retried. One genuine race is refused: the old-server form (LOW-2). That
   failure is closed.

2. **Can a race retry loop forever, or push something unintended?**

   No to both.

   - The loop is bounded by `$MaxPushTries` (default 3).
   - Every attempt after the first re-fetches. Every attempt then:
     - rebases (aborting cleanly on conflict);
     - re-runs the narrow post-rebase checks, plus the KNOWN-failure re-judge;
     - re-runs the pin check, which is skipped only when the gitlinks are unchanged since a pass;
     - pushes `HEAD:<target>` without force.

   Caveats:

   - The narrow re-check is the pre-existing, documented design. The full `-CheckLog` stays bound to
     the pre-rebase tree.
   - The re-check on retry and the attempt count are not regression-tested (LOW-3).

3. **Does the kill-lint allowlist have a bypass?**

   Yes: nine verified, three confirmed by negtest (LOW-1). Its own self-tests cannot detect a
   weakened lint (MED-1).

4. **Are the other land.ps1 paths regressed? And why can a NEW failure land?**

   **No regression.** The diff touches only the push block and the classifier:

   - `-Target main` still requires both `-Coordinator` and `-CheckLog`.
   - `-AllowKnownFailures`, the `-CheckLog` tree binding, `-AllowFail` and worktree removal are
     unchanged.
   - `LC_ALL`/`LANG` are restored in a `finally`.

   **NEW failures with `-CheckLog` absent:** there is no gate at all. This is the dev-flow intent:

   - `docs/agent_rules/COMMON.md:36`: agents run targeted tests only, never `run_all_checks`, and
     pass `-CheckLog` only if they have one.
   - `docs/MCP_SERVERS.md`: "the check-log gate is optional for dev".
   - The coordinator full-runs the dev tip, and `dev_promote.ps1 -Push` requires a clean,
     unfiltered `-CheckLog` with no NEW failures before main moves.

   So a NEW failure can reach dev by design, but never main.

   **NEW failures with `-CheckLog` present:** a NEW failure is refused. It can still land in two ways:

   - an `-AllowFail` regex matches it. The match is an unanchored substring, so it can be broader
     than intended.
   - the failure appears only after rebasing onto a moved dev tip. Only the narrow re-check runs
     then.

   A timed-out check prints `FAIL x (timed out ...)`, which the FAIL regex does not parse. The
   FAIL-count mismatch check (F10) still refuses it. `FAILED:` lines (skips, BUSY) refuse.

5. **Mutation evidence:** see the tables below. Every CAUGHT log was inspected. Each has real
   `  FAIL: <assert text>` lines from the check's `Assert` and a final `check_*: N FAILED` line, and
   none has a parse error.

## Mutation evidence

`tools\negtest.ps1 -Preset check -PresetArg tools\check_land.ps1 -Parallel 4`
(baseline passed in 1093 s, under heavy machine load; `real_tree_unchanged: true`)

| Mutation | Verdict | Assert evidence |
|---|---|---|
| `land_no_lc_all` | MISSED | equivalent on this host (INFO-8) |
| `verdict_remote_rejected_all_race` | CAUGHT | 13 FAILs, e.g. "verdict hook declined -> refused", "no retry loop on remote rejection (rebases: 3)" |
| `verdict_short_sha` (`{40}` -> `\S+`) | CAUGHT | "verdict short sha is not git form -> refused" |
| `verdict_drop_caret` | MISSED | test gap, not equivalent (LOW-3) |
| `race_not_retried` | CAUGHT | 9 FAILs, e.g. "landed after real-race retry", "rebased twice (got 1)" |
| `refused_to_other` | CAUGHT | 4 FAILs, e.g. "stale lock: refused with its message" |
| `no_porcelain` | CAUGHT | 13 FAILs, e.g. "landed after retry", "gitlink racer: pin check ran 1 time(s), want 2" |
| `fetch_first_not_race` | CAUGHT | 7 FAILs, e.g. "verdict fetch first -> race" |
| `no_refetch_on_retry` | CAUGHT | 6 FAILs, e.g. "rebased twice (got 3)" |
| `maxtries_default_5` | MISSED | no attempt-count assert (LOW-3) |
| `recheck_only_first_attempt` | MISSED | no re-check-on-retry assert (LOW-3) |

`tools\negtest.ps1 -Preset check -PresetArg tools\check_push_verify.ps1 -Parallel 3`
(baseline passed in 311 s; `real_tree_unchanged: true`)

| Mutation | Verdict | Assert evidence |
|---|---|---|
| `lint_drop_cmd_from_bad` | MISSED | self-tests vacuous (MED-1) |
| `pv_no_job_ungated` (LOW-4 gate reverted) | CAUGHT | "stray PUSH_VERIFY_TEST_NO_JOB without PUSH_VERIFY_TEST_MODE: ignored (job path used)" |
| `pv_timeout_cap` (2,000,000 -> 2,000,001) | CAUGHT | "oversized -FetchTimeoutSec -> usage error (INFO-4)" |
| `control_plain_stop_process` | CAUGHT | `lint: forbidden command: Stop-Process`, then "S2/L1: kill-surface allowlist holds ..." |
| `bypass_module_qualified` | MISSED | lint bypass (LOW-1) |
| `bypass_single_quoted_addtype` | MISSED | lint bypass (LOW-1) |
| `bypass_wmi_foreach_terminate` | MISSED | lint bypass (LOW-1) |
