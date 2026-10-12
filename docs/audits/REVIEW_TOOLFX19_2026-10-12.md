# Review: TOOLFX19 (b6b4b34e2) -- 2026-10-12

Adversarial review of commit `b6b4b34e2` on origin/dev ("wt_status: CLI -Prune
-WhatIf check, loud missing-type guard; worktree_mint: retry, verify and fail
loudly on submodule init"). Scope: this commit only, reviewed in a worktree at
`b6b4b34e2`. Files: `tools/wt_status.ps1`, `tools/check_wt_status.ps1`,
`tools/worktree_mint.ps1`.

## Verdict

No HIGH. One MED, in the check, not in the tools: the standing check passes
green when `wt_status.ps1` throws. It existed before this commit, but the new
type guard is exactly the kind of throw that it hides. The three questions
asked:

- **Can `-WhatIf` delete anything?** No. The only removal paths
  (`worktree_mint -Remove`, `Remove-TreeSafe`) sit after the
  `if ($WhatIf) { ... continue }` at `tools/wt_status.ps1:439`, and the closing
  `git worktree prune` becomes `-n -v` under WhatIf (`:459`). WhatIf still runs
  read-only git queries (`git status`, `cherry`, `log`) in each worktree, and
  `git status` can refresh that worktree's index stat cache. That is a
  pre-existing, non-destructive write. Negtest M1 (WhatIf honoured only with
  `-PassThru`) proves the new CLI case would catch a CLI-only WhatIf bypass.
- **Can the mint retry corrupt a shared `.git/modules`, or exit wrongly?**
  It cannot corrupt one. Submodule git dirs are per-worktree
  (`tools/mykicadMcp/.git` -> `.git/worktrees/<wt>/modules/mykicadMcp`,
  checked in the minted worktree). The only shared state that init writes is
  the common `.git/config`, and git updates that through a lockfile and an
  atomic rename. The comment at `tools/worktree_mint.ps1:307` ("share one
  .git/modules store") is therefore wrong, but harmless (INFO-1). The exit code
  is wrong in two cases, L2 and L3 below.
- **Is the new check case vacuous?** No. Negtest M1, M2, M3, M4 and M7 were
  CAUGHT by the new assertions. The PassThru cases could not have caught any of
  them. The new type guard itself is not covered: removing it (M6) is MISSED.

## Findings

### MED-1 -- `check_wt_status.ps1` passes green when `wt_status.ps1` throws (fail-open)

`tools/check_wt_status.ps1:54` wraps the whole fixture and assertion body in
`try { ... } finally { ... }` with no `catch`, and nothing sets a minimum
assertion count (`:274-280`). Suppose `wt_status.ps1` raises a terminating
error out of `Run-Tool` (`:161`). The finally block cleans up, the statement
ends, the failure list is still empty, and the check prints
`wt_status check passed: 4 assertions` with exit 0.

Negtest M5 shows this. It makes this commit's own guard
(`tools/wt_status.ps1:238`) fire everywhere, and the check reported PASS with 4
assertions instead of 138 (log `m05_3jz1.log`). So any future wt_status
regression that throws in PassThru mode would read green in the standing
suite. That includes the exact "type missing" state this commit's guard is
meant to make loud.

The fail-open is older than this commit. It is rated MED because the commit
adds a new throw path and says it is covered.

Fix: add `catch { $failures.Add("check aborted: $($_.Exception.Message)") }`
to the main `try`. Also add an assertion-count floor, for example
`Assert-True ($assertions -ge 130)`, or count the sections reached.

### LOW-1 -- The type guard is loud in text only; wt_status still exits 0, and the guard is untested

`tools/wt_status.ps1:238` throws inside the pooled runspace. `EndInvoke`
(`:361`) then raises a statement-terminating error, which aborts the
fact-gathering `foreach`. The rest of the script continues, every row becomes
UNKNOWN ("could not be inspected"), nothing is pruned, and the process **exits
0**. The CLI output from M4 shows one red `EndInvoke` error, then
`UNKNOWN=21`, then `nothing to prune`, and `$cliRc` was 0.

The behaviour is safe, and it is a real improvement: before the guard, a
missing type degraded silently. A probe confirmed this:
`[NoSuchType]::Walk()` inside an `AddScript` runspace with
`ErrorActionPreference=Continue` logs one error and keeps going. Facts then
come back with `Kicad=$false` and only the top-level directory's mtime, so an
unregistered directory holding `.kicad_*` files, or a recently touched deep
file, could be classed ORPHAN_DIR. That can happen when `Add-Type` itself fails
(compiler unavailable, temp directory blocked), because the script carries on
under `Continue`.

There are two gaps:

- An automated caller still sees exit 0.
- Nothing tests the guard. Deleting it (M6) is MISSED, because every fixture
  run has the type loaded.

Fix:

- Check `'WtStatusWalk' -as [type]` once in the main session right after the
  `Add-Type` block (`:67-182`) and `exit 1` if it is missing. Pooled runspaces
  share the AppDomain, so the in-runspace guard then becomes belt and braces.
- Add a CLI child-process case that runs a copy whose C# block does not
  compile, and asserts a nonzero exit and no `would remove` rows. A child
  process is needed because a loaded type persists in the check's AppDomain.

### LOW-2 -- The mint now hard-fails on any submodule, including the vendor-only one

`tools/worktree_mint.ps1:312-320` fails the mint whenever
`git submodule update --init --recursive` exits nonzero after four tries.
`--recursive` also covers `hardware/mainBoard/parts/TFT35-SPI`, a
third-party GitHub repo. The header (`:87-91`) says no check uses that
submodule. If that upstream repo becomes unreachable, or a network flake
outlasts about 20 s of retries, every agent's default mint exits 1, even when
lvgl and mykicadMcp are fine.

The two gates also cover different submodules:

- The exit-code gate covers all three submodules.
- The populated gate (`:322`) checks only `tools/mykicadMcp`. lvgl is not
  checked, yet the header names it as required by every KilnFW target build.

Fix: init the required submodules (lvgl and mykicadMcp) as hard requirements,
verify both, and keep TFT35-SPI best-effort with a warning.

### LOW-3 -- A hardcoded `tools/mykicadMcp` makes a mint of an older `-Base` fail

The submodule moved from `mykicadMcp/` to `tools/mykicadMcp` in `164bd324a`
(2026-08-28). A worktree minted at an older `-Base`, for example for bisecting,
initialises its submodules successfully. The verification at
`tools/worktree_mint.ps1:322` still finds no `tools/mykicadMcp/.git` and exits
1, so a correct mint is reported as failed.

Fix: derive the paths to verify from the new worktree's own `.gitmodules`
(`git -C $target config -f .gitmodules --get-regexp path`). Alternatively, use
`git submodule status --recursive` and treat a leading `-` or `+` as
unpopulated.

### LOW-4 -- A failed mint leaves a registered worktree that the caller cannot name; the docs are now stale

On failure the mint exits before it prints `WORKTREE:`
(`tools/worktree_mint.ps1:325-329`; `WORKTREE:` is printed only at `:336`). Callers that parse that line, such as
`tools/main_baseline.ps1:47-49`, report "worktree mint failed" and lose the
path. The registered worktree is then leaked until `wt_status -Prune` treats
it as STALE_CLEAN two hours later. The path is printed only in prose.

`docs/MCP_SERVERS.md:789-790` still says a submodule-init failure "never fails
the mint itself -- the worktree is still handed back". That now contradicts
the code.

Fix: either remove the worktree just created before `exit 1`, or print a
machine-readable `WORKTREE_FAILED: <path>` line. Update the
`MCP_SERVERS.md` paragraph either way.

`tools/check_worktree_mint.ps1` runs every case with `-NoSubmodules`, so none
of the new retry, verify or exit-1 code is exercised.

### INFO

- **INFO-1:** The comment at `tools/worktree_mint.ps1:307-308` is wrong about
  what is shared. `.git/modules` is per-worktree
  (`.git/worktrees/<wt>/modules`). The shared, lock-raced file is the common
  `.git/config`. The retry is still justified for `config.lock`.
- **INFO-2:** The retry loop sleeps and prints "retrying ..." after the fourth
  and final failure too (`:316-317`). That wastes 8 s and misleads the reader.
- **INFO-3:** The commit silently drops the UTF-8 BOM from `tools/wt_status.ps1`
  (line 1 of the diff). The file is pure ASCII (0 non-ASCII bytes), so
  PowerShell 5.1 parses it the same way, but the commit message does not
  mention the change.
- **INFO-4:** The type guard checks only that the type exists, not its
  version. A long-lived session that loaded an older `WtStatusWalk` (same
  name, different members) skips `Add-Type` (`:67`) and uses the stale type.
  This is rare: CLI runs are always fresh processes.

## Negtest table

Run with `tools\negtest.ps1 -Preset check -PresetArg tools/check_wt_status.ps1
-Mutations <json>`, with a per-mutation `expect` pinned to the specific
assertion message.

- **Baseline:** PASS in 198.8 s, 138 assertions.
- **`real_tree_unchanged`:** true. **`copies_removed`:** true. No
  `REAL TREE CHANGED`.
- **Logs:** `%TEMP%\negtest_logs\20261012_134234_m2i1\`.

Every CAUGHT below is a failing `Assert-True` message from the check's
`WT_STATUS CHECK FAILED` list, not a crash.

| # | Mutation (`tools/wt_status.ps1`) | Verdict | Failing assertion(s) |
|---|---|---|---|
| M1 | `:439` WhatIf honoured only with `-PassThru` (the CLI really prunes) | CAUGHT | `CLI -Prune -WhatIf preview missing 'would remove' row` x5, `CLI WhatIf removed or created something under the root` |
| M2 | `:462` WhatIf header text changed | CAUGHT | `CLI -Prune -WhatIf printed no WhatIf preview` |
| M3 | `:473` `$failed -gt 0` -> `-ge 0` (CLI always exits 1) | CAUGHT | `CLI -Prune -WhatIf exited 1` |
| M4 | `:67` `Add-Type` only when `-PassThru` (type missing in a fresh CLI process) | CAUGHT | `CLI -Prune -WhatIf hit a type error`, `... missing 'would remove' row` x5 |
| M5 | `:238` guard always fires (type name typo) | **MISSED** | none. The check printed `passed: 4 assertions` and exited 0 (MED-1). |
| M6 | `:238` guard line deleted | MISSED (expected) | none. The fixture never lacks the type (LOW-1). |
| M7 | `:463` preview table reduced to `Name` only | CAUGHT | `CLI -Prune -WhatIf preview missing 'would remove' row` x5 |

## Counts

HIGH 0, MED 1, LOW 4, INFO 4.
