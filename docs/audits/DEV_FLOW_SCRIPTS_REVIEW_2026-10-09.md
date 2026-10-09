# Dev-flow git/release scripts review (2026-10-09)

Scope: the git and release workflow scripts as of origin/dev `1e6d375f`, and
their tests.

- `tools/land.ps1`, `tools/dev_promote.ps1`, `tools/release_merge.ps1`
- `tools/worktree_mint.ps1`, `tools/push_verify.ps1`, `tools/commit_guard.ps1`
- `tools/wt_status.ps1`, `tools/main_baseline.ps1`, `tools/main_baseline_lib.ps1`
- Tests: `tools/check_land.ps1`, `tools/check_dev_promote.ps1`,
  `tools/check_worktree_mint.ps1`, `tools/check_wt_status.ps1`,
  `tools/check_main_baseline.ps1`

This is a review only. No code was changed. Line numbers refer to `1e6d375f`.

## Summary

| # | Sev | Area | Finding |
|---|-----|------|---------|
| F1 | HIGH | land.ps1 | `-Target main` pushes straight to main with no required check log |
| F2 | MED | dev_promote.ps1 | Promote has no test-evidence gate; any dev ancestor can reach main |
| F3 | MED | land.ps1 | `-RestartMcp` calls the undefined `Run-Bounded`; always exits 1, never restarts |
| F4 | MED | worktree_mint.ps1 | `-Remove` deletes a detached worktree whose commits exist nowhere else -- FIXED (worktree_mint -Remove refuses on `rev-list HEAD --not --remotes --branches` unless -Force; lists the shas) |
| F5 | MED | wt_status.ps1 | `-Prune` deletes gitignored captured data, mid-rebase worktrees, and cwd-only sessions -- FIXED (gitignored data under logs/ or elf_archive/ and mid-rebase/cherry-pick/merge/revert are HAS_WORK; the cwd-only case is not detectable from PS 5.1, documented in the script header) |
| F6 | MED | main_baseline_lib.ps1 | Baseline lineage never matches in the dev flow; KNOWN/`-AllowKnownFailures` inert |
| F7 | MED | push_verify.ps1 | `-Branch dev` (no remote prefix) checks a local ref; can print LANDED for an unpushed commit |
| F8 | MED | commit_guard.ps1 | Never fetches; compares against a stale local origin/dev |
| F9 | LOW | land.ps1 | Check log not bound to HEAD/tree; first summary in an appended log wins |
| F10 | LOW | land.ps1 | FAIL-line parse can under-count; failedCount not reconciled |
| F11 | LOW | land.ps1 | Any `rejected` (hook / protected branch) is retried as a non-fast-forward |
| F12 | LOW | dev_promote.ps1, release_merge.ps1 | Post-push "verify" is tautological; a racing push gives a false failure |
| F13 | LOW | release_merge.ps1 | Release push and tag push are not atomic; wrong default trailer model -- FIXED (one `git push --atomic` of release + tag, local tag dropped on rejection; default trailer Opus 5.5) |
| F14 | LOW | worktree_mint.ps1 | `-Remove` path handling: cwd-relative GetFullPath, wildcard Test-Path, junction unlink before git validates -- FIXED (Resolve-Path -LiteralPath, linked-worktree check via `git worktree list` before any unlink) |
| F15 | LOW | wt_status.ps1 | 200-commit cap on ahead list; stale header text says origin/main -- FIXED (no cap on the unlanded count; header names the real base) |
| F16 | LOW | main_baseline.ps1 | `-Record` Start-Process arguments are unquoted (paths with spaces) -- FIXED (-Record quotes the run_all_checks path) |
| F17 | LOW | commit_guard.ps1 | `-File` comma arrays, binary numstat, deleted files |
| F18 | LOW | tests | No tests for release_merge, push_verify, commit_guard; land tests default to `-Target main` -- PARTLY FIXED (check_worktree_mint -Remove cases, check_release_merge.ps1, wt_status stale_ignored flipped; push_verify/commit_guard/land/dev_promote/main_baseline gaps remain) |

Things checked and found sound are listed at the end.

## Findings

### F1 HIGH -- land.ps1 can put an untested commit on main

- Where: `tools/land.ps1:64` (`[ValidateSet('dev','main')][string]$Target = 'dev'`),
  `tools/land.ps1:172-248` (check-log gate runs only when `-CheckLog` is passed),
  `tools/land.ps1:302` (`git push origin HEAD:$Target`).
- Scenario: an agent copies an older landing recipe, or a stale doc, and runs
  `tools\land.ps1 -Target main` without `-CheckLog`. After the rebase the only
  checks that run are the narrow re-checks (`check_mcp_tool_count_doc`,
  `check_mcp_facade_coverage`). The push goes straight to origin/main. That
  bypasses the coordinator's full run on the dev tip and the single-parent
  squash that `dev_promote.ps1` builds, and puts a multi-parent or untested
  history on main. `check_land.ps1`'s `Run-Land` helper defaults to
  `-Target main`, so the test suite exercises this path as the normal case.
- Fix: remove `main` from the ValidateSet, so only `dev_promote.ps1` writes
  main. If a direct-to-main escape hatch must stay, require both an explicit
  coordinator switch (for example `-IAmCoordinator`) and a `-CheckLog` whose
  recorded HEAD/tree equals the commit being pushed (see F9). Change the test
  default to `-Target dev`.

### F2 MED -- dev_promote.ps1 has no test-evidence gate

- Where: `tools/dev_promote.ps1:48-56` (Test-RealPromote), `:59-70` (offender
  check), `:91-96` (commit-tree), `:107` (push).
- Scenario: the promote checks that `-Commit X` is on origin/dev and that main
  has no commits outside the dev flow. It does not check that `X` was ever
  tested. A coordinator who passes the dev tip that was pushed a minute ago,
  rather than the one the full run covered, promotes untested code to main.
  Nothing binds `-Commit` to a `run_all_checks.ps1` result.
- Fix: require a check-log argument, or a `C:\wt\.checkcache` /
  `C:\wt\.mainbaseline` record, whose tree hash equals `X^{tree}`, whose mode is
  full (or `-Fast` plus an explicit opt-in), and which reports 0 NEW failures.
  Refuse without it unless the caller passes an explicit override that is
  recorded in the promote commit message. Add a `check_dev_promote.ps1` case
  for "untested commit refused".

### F3 MED -- land.ps1 -RestartMcp always fails and never restarts

- Where: `tools/land.ps1:327` and `:330` call `Run-Bounded`. That function is
  not defined anywhere in the script or in anything it dot-sources. It has been
  missing since `eebf333e`.
- Scenario: `land.ps1 -RestartMcp` lands the commit, then calling
  `Run-Bounded` raises `CommandNotFoundException`. `$ErrorActionPreference` is
  not Stop, so the script continues with `$rc = $null`; `$null -ne 0` is true
  and the optional-step failure text becomes `mcp_servers.ps1 restart exited `
  (empty code). The script exits 1 with `landed=true`, and the MCP servers keep
  serving stale code. Callers that treat exit 1 as "not landed" may retry the
  land. This was reproduced in PowerShell 5.1 during this review.
- Fix: define `Run-Bounded` (Start-Process with a WaitForExit timeout, a
  process-tree kill on timeout, and the exit code returned), or call
  `mcp_servers.ps1` directly. Add a `-RestartMcp` case to `check_land.ps1`
  using a stub `mcp_servers.ps1`. That missing case is why this went unnoticed.

### F4 MED -- worktree_mint.ps1 -Remove loses unlanded detached commits

- Where: `tools/worktree_mint.ps1:137` refuses only when
  `git status --porcelain --untracked-files=all` is non-empty. `:157` always
  passes `--force` to `git worktree remove`.
- Scenario: an agent commits in its detached worktree, and the push is rejected
  or the session ends before it lands. The tree is clean, so `-Remove`
  proceeds. A detached HEAD has no branch, and removing the worktree deletes its
  reflog (`.git/worktrees/<name>/logs/HEAD`). The commits become unreachable and
  are gc-eligible. `wt_status.ps1` calls this same `-Remove` for prunes, though
  its own classification would normally keep HAS_WORK.
- Fix: before removal, refuse when
  `git -C <wt> rev-list HEAD --not --remotes --branches` is non-empty, and when
  `git cherry <base> HEAD` shows a `+` line, unless `-Force` is given. Print the
  shas being dropped either way.

### F5 MED -- wt_status.ps1 -Prune can delete data and live sessions

- Where: `tools/wt_status.ps1:215` (dirty/untracked counts use plain
  `status --porcelain`, so gitignored files are invisible), `:186-192` (process
  match on command line and executable path only), STALE_CLEAN classification,
  and the prune path that calls `worktree_mint.ps1 -Remove`.
- Scenarios:
  1. A worktree holds captured run data under gitignored paths
     (`logs/coupling/*`, `logs/bench_test/*`, `firmware/KilnFW/elf_archive/`).
     The tree reads clean, HEAD is on origin/dev, it has been idle more than
     2 h: it is STALE_CLEAN, and `-Prune` deletes the data. CLAUDE.md lists
     these paths as "never delete". `check_wt_status.ps1`'s `stale_ignored`
     case asserts the prune happens, so the test pins the unsafe behaviour.
  2. A worktree is mid-rebase or mid-cherry-pick with a conflict-free, clean
     index (for example a stopped `rebase` after `git rebase --continue` failed
     on a hook). The original commits live only in `ORIG_HEAD`, `rebase-merge/`
     or the reflog. The worktree reads STALE_CLEAN and the prune loses them.
  3. A read-only review session (like this one) has its cwd in the worktree but
     no path in its command line and no file edits for more than 2 h. No process
     match, idle time high: STALE_CLEAN, removed out from under the session.
- Fix: count `git status --porcelain --ignored` entries under a protected-path
  list (logs/, elf_archive/, *.tsv captures) and class those HAS_WORK. Class as
  UNKNOWN any worktree with `rebase-merge`, `rebase-apply`, `CHERRY_PICK_HEAD`,
  `MERGE_HEAD` or `REVERT_HEAD` in its git dir. For liveness, also honour a
  marker file the minting session refreshes (for example `.wt_alive`, touched by
  the session), since PowerShell 5.1 cannot read another process's cwd cheaply.
  Flip the `stale_ignored` test to expect KEEP.

### F6 MED -- main baseline lineage is inert in the dev flow

- Where: `tools/main_baseline_lib.ps1:258-292` (Select-MainBaseline requires the
  baseline commit to be an ancestor of HEAD and prefers the merge-base with
  origin/main).
- Scenario: with dev/main split, main moves only by promote commits
  (`129586d4`, `c672e509`, `3a210085`), which are single-parent squashes and
  never ancestors of dev. `git merge-base origin/dev origin/main` is stuck at
  `676c503f`, the pre-split commit. No baseline recorded on a promote commit can
  ever be Exact, so on any dev-based tree every failure is NEW. That makes
  `run_all_checks.ps1 -FailOnlyOnNew` and `land.ps1 -AllowKnownFailures`
  no-ops. This fails closed, so no bad code lands because of it, but it pushes
  agents toward ignoring the NEW list or bypassing the gate.
- Fix: record baselines against the dev sha the promote names (the promote
  subject already carries `Promote dev <sha>`), and resolve a promote commit
  back to that sha before the ancestor test. Or record baselines on the dev tip
  the coordinator full-runs. Add a `check_main_baseline.ps1` case with a
  squash-promoted main.

### F7 MED -- push_verify.ps1 can report LANDED for an unpushed commit

- Where: `tools/push_verify.ps1:46` (default `origin/dev`), `:73` (remote parsed
  from an `x/` prefix), `:95` (`git merge-base --is-ancestor $fullHash $Branch`
  with `$Branch` used raw).
- Scenario: an agent passes `-Branch dev`. The ancestor test then runs against
  the LOCAL `dev` branch. If the agent committed on a local `dev` and the push
  failed, the commit is an ancestor of local `dev` and the script prints LANDED.
  A ref named `refs/heads/origin/dev` (easy to create with a typo'd
  `git branch`) would likewise shadow the remote-tracking ref for the default.
- Fix: accept only `<remote>/<branch>`, resolve `refs/remotes/<remote>/<branch>`
  explicitly, and refuse anything else. Optionally confirm with
  `git ls-remote <remote> refs/heads/<branch>` so the check does not depend on
  the local tracking ref at all.

### F8 MED -- commit_guard.ps1 compares against a stale origin/dev

- Where: `tools/commit_guard.ps1` has no `git fetch`; `:104` compares against
  `rev-parse "${Branch}:${p}"` from the local tracking ref.
- Scenario: another session lands a change to the same file after this
  worktree last fetched. The guard compares against the old blob, sees only
  this session's edits, and passes. The commit then carries a silent revert of
  the other session's change into the rebase. That is the exact
  shared-tree trap the guard exists to catch.
- Fix: `git fetch origin <branch>` at the start and refuse when the fetch
  fails (check `$LASTEXITCODE`, not `$?`, and do not redirect native stderr
  into the pipeline under PowerShell 5.1).

### F9 LOW -- land.ps1 check log is not bound to the commit

- Where: `tools/land.ps1:126` (`$SummaryRe`, first match wins), `:172-248`.
- Scenario: an agent passes a check log from an earlier run in another
  worktree, or a log with several runs appended. The gate reads the first
  summary, which may be green, and accepts it for a different tree.
- Fix: have `run_all_checks.ps1` print the HEAD sha and tree hash in its
  summary, and have `land.ps1` require that the tree matches the pre-rebase
  HEAD tree. Use the last summary in the file, not the first.

### F10 LOW -- land.ps1 FAIL-line parsing can under-count

- Where: `tools/land.ps1:205` (the FAIL regex requires a `(`), `:244` (the
  failedCount fallback fires only when zero FAIL lines were parsed).
- Scenario: a run with 3 failures where one FAIL line has no parenthesised
  detail parses 2 lines. The fallback does not fire, and the NEW/KNOWN split is
  computed from 2 of 3 failures. With F6 fixed and `-AllowKnownFailures` live,
  the unparsed failure would be silently accepted.
- Fix: compare the parsed count against the summary's failedCount and refuse on
  any mismatch.

### F11 LOW -- land.ps1 retries a hook or protected-branch rejection

- Where: `tools/land.ps1:302-304` (retry when the push output matches
  `rejected`).
- Scenario: the remote refuses with `! [remote rejected] HEAD -> dev (pre-receive
  hook declined)`. The loop treats it as a non-fast-forward race, re-fetches,
  rebases and pushes again until it runs out of attempts, and then reports a
  race rather than the real cause.
- Fix: retry only on `(fetch first)` or `(non-fast-forward)`. Fail immediately
  with the remote's message on `remote rejected`.

### F12 LOW -- promote/release post-push verify is tautological

- Where: `tools/dev_promote.ps1:110`, `tools/release_merge.ps1:126`.
- Scenario: a successful `git push` already updates
  `refs/remotes/origin/main`, and the re-fetch's exit code is not checked, so
  comparing that ref to `$m` proves nothing beyond the push's own exit code. If
  another promote lands between the push and the fetch, the compare fails and
  the script reports failure for a push that did land.
- Fix: verify with `git merge-base --is-ancestor $m origin/main` after a checked
  fetch (or `ls-remote`), which is true both for an exact match and after a
  later push.

### F13 LOW -- release_merge.ps1 partial state and trailer

- Where: `tools/release_merge.ps1:119-124` (push release, then create and push
  the tag), `:22` (default trailer `Claude Sonnet 5.5`), `:31` (GOk without a
  stderr redirect).
- Scenario: the release branch push succeeds and the tag push fails (network,
  existing tag). origin/release has the squash commit with no tag, and a re-run
  computes "nothing to release". The daily tag is then missing until fixed by
  hand.
- Fix: push both in one atomic push (`git push --atomic origin
  <sha>:refs/heads/release refs/tags/<tag>`). Make the default trailer the
  session's model or require it as a parameter.

### F14 LOW -- worktree_mint.ps1 -Remove path handling

- Where: `tools/worktree_mint.ps1:129` (`[System.IO.Path]::GetFullPath($Path)`),
  `:130` (Test-Path without `-LiteralPath`), `:156` (reparse points stripped
  before git confirms the path is a linked worktree).
- Scenarios: `GetFullPath` resolves a relative path against the process cwd,
  not PowerShell's location, so `-Remove .\foo` after `Set-Location` targets
  the wrong directory. A path containing `[` is treated as a wildcard. Pointed
  at the main tree by mistake, the junction unlink step runs before
  `git worktree remove` refuses, unlinking junctions in the main tree.
- Fix: resolve with `(Resolve-Path -LiteralPath $Path).ProviderPath`, use
  `-LiteralPath` throughout, and confirm the path appears in
  `git worktree list --porcelain` (and is not the main worktree) before
  touching anything.

### F15 LOW -- wt_status.ps1 ahead-list cap and stale header

- Where: `tools/wt_status.ps1:204` (`log -n 200 $base..HEAD`), `:24-25`, `:41`
  (header says origin/main; `-Base` defaults to origin/dev).
- Scenario: a long-lived branch with more than 200 commits ahead reports only
  200 for the cherry check. The header text misleads readers about the base.
- Fix: drop the cap for the unlanded count (keep it only for display), and fix
  the header text.

### F16 LOW -- main_baseline.ps1 -Record unquoted arguments

- Where: `tools/main_baseline.ps1:52-55` (Start-Process with an `-ArgumentList`
  array).
- Scenario: PowerShell 5.1's Start-Process joins the array with spaces and does
  not quote elements. Today the paths are under `C:\wt\`, so no space appears.
  If the script or log path ever contains a space (the main tree lives under
  `OneDrive\Desktop`), the child gets split arguments.
- Fix: quote each path element explicitly (`"`"$path`""`) or pass a single
  pre-quoted argument string.

### F17 LOW -- commit_guard.ps1 input edge cases

No release_merge part exists in this finding; commit_guard.ps1 is handled separately and stays open here.


- Where: `tools/commit_guard.ps1` parameters; `:93` (Test-Path on each path);
  the numstat parse.
- Scenarios: the documented `-Path a,b -ExpectedMaxLines 40,120` arrives as one
  string under `powershell -File`, so the guard fails closed with a confusing
  error. A binary file's numstat is `-`, parsed as 0 lines changed, so any binary
  change passes the line limit. A deleted file fails the Test-Path and cannot be
  guarded at all.
- Fix: split comma strings explicitly; treat `-` as "binary, must be
  confirmed"; allow deleted paths by checking `git ls-files` / the base tree
  instead of the working tree.

### F18 LOW -- test coverage gaps

- No test exists for `release_merge.ps1`, `push_verify.ps1` or
  `commit_guard.ps1` beyond `check_worktree_mint.ps1`'s default-branch text
  match.
- `check_land.ps1`: `Run-Land` defaults to `-Target main` (F1); no
  `-RestartMcp` case (F3); no hook-rejection case (F11); no stale-log binding
  case (F9).
- `check_dev_promote.ps1`: no untested-commit case (F2); no concurrent-promote
  case.
- `check_wt_status.ps1`: `stale_ignored` asserts the unsafe prune (F5); no
  mid-rebase or cwd-only-session case.
- `check_worktree_mint.ps1`: no `-Remove` cases at all (F4, F14).
- `check_main_baseline.ps1`: no squash-promoted main topology (F6).

## Checked and not defects

- Force push: no script force-pushes a shared branch. Every push is a plain
  fast-forward refspec. `check_dev_promote.ps1` uses `--force` only against its
  own scratch origin.
- Concurrent landers: two sessions running `land.ps1` at once are serialised by
  the remote's non-fast-forward rejection plus the fetch/rebase/retry loop.
  The loser rebases onto the winner and re-runs the narrow checks.
- Concurrent promotes: the second `dev_promote.ps1` push is rejected as
  non-fast-forward and the script fails rather than overwriting.
- `push_verify.ps1` with the default `origin/dev` after a checked fetch is
  direction-safe and checks `$LASTEXITCODE` rather than `$?`.
- `wt_status.ps1` re-classifies each candidate immediately before removal and
  routes removal through `worktree_mint.ps1 -Remove`, so it is junction-safe.
