# Review: negfx commits (negtest process reaping, -RequireAssertion, L6, push_verify retry), 2026-10-10

Commits reviewed on `origin/dev` (base `7f5884db4`):

- `93df083f2`: negtest adopts only children created at or after their parent, drops `taskkill /T`
  from `Stop-Tracked`, spares shared daemons on every kill; `-RequireAssertion` matches the KilnFW
  `  FAIL file:line:` form; `push_verify.ps1` retries the temp-file delete.
- `44ecaf482`: bare `-Preset check` names resolve through `git ls-files` (tracked files only).
- `7f5884db4`: marks REVIEW_FIREFX3_TOOLFX2 B-MED-1/B-LOW and REVIEW_R2ACE_TOOLFX4 L6 fixed.

Review only. No code changed.

## Verdict

The B-MED-1 fix is correct where it applies. `Add-Descendants` and `Stop-Tracked` can no longer
adopt or kill a process older than its reported parent, and `Stop-Tracked` re-checks identity
before it kills. The L6 fix works: an untracked duplicate no longer makes a name ambiguous, and
tracked duplicates are still reported. The `push_verify.ps1` retry is correct and cannot change
the verdict.

Open issues:

- One `taskkill /T` path is still unfixed. `Stop-Tree` runs on the root and on worker processes in
  the timeout and abort paths (LOW-1).
- Test discrimination is weak. 4 of 5 negtest mutations on the new negtest code were MISSED, and
  both push_verify mutations were MISSED (LOW-3).

No HIGH or MEDIUM findings.

## Evidence

- `tools/check_negtest.ps1` on the dev tip: `negtest check passed: 215 assertions across 11 parallel groups`, exit 0.
- `tools/negtest.ps1 -Preset check -PresetArg tools\check_negtest.ps1 -Parallel 2`. The baseline
  passed, `real_tree_unchanged=True`, `copies_removed=True`.

  | mutation | change | verdict |
  |---|---|---|
  | L6_include_untracked | `ls-files --` -> `ls-files --cached --others --` | MISSED |
  | L6_no_ambiguity | `$hits.Count -gt 1` -> `-gt 99` | MISSED |
  | adopt_bypass_keeps_string | `if ($false -and -not (Test-ChildAdoptable $c $parentCreated))` | MISSED |
  | copyproc_tree_kill | `Stop-CopyProcesses` back to `taskkill.exe /T /F` | MISSED |
  | reqassert_drop_kilnfw_arm | remove the `(?m)^\s+FAIL \S+:\d+:` alternative | CAUGHT |

- `tools/negtest.ps1 -Preset check -PresetArg tools\check_push_verify.ps1`: the baseline passed.
  `pv_retry_once` (`$i -lt 5` -> `-lt 1`) and `pv_no_break` (drop `break`) were both MISSED.
- L6 by hand, in this review's worktree. With an untracked copy at
  `logs/rvnegfx_tmp/check_push_verify.ps1`, `-PresetArg check_push_verify.ps1` resolved
  unambiguously: the run went on to mutation validation (`find string matches 0 time(s)`). After
  `git add` of the same copy it stopped with `ambiguous check name check_push_verify.ps1 matches:
  logs\rvnegfx_tmp\check_push_verify.ps1, tools\check_push_verify.ps1`. The fixture was unstaged
  and deleted afterwards.

## Findings

### LOW-1: `Stop-Tree` (`taskkill /T`) on the root and on workers still walks stale parent PIDs

`tools/negtest.ps1:331` (`Stop-Tree`), which is called at:

- `:479` (timeout)
- `:572` (Invoke-Chunk finally)
- `:904` (live workers in the top-level finally)
- `:905` (top-level finally)

The same pattern is in `tools/push_verify.ps1:107`, on fetch timeout.

**Scenario.** `taskkill /T` builds its tree from `ParentProcessId` alone. It is not documented to
compare creation times; this review did not prove either way. The root `cmd.exe` is alive on the
abort paths (`:572`, `:905`) and on `:904`. Suppose an unrelated long-lived process V has a stale
PPID equal to the root's PID, because V's real parent held that PID earlier and exited. Then
`taskkill /T` kills V, and then V's whole subtree, recursively, with no time check. This is
B-MED-1 case (a), which the fix closed only for `Add-Descendants` and `Stop-Tracked`.

On the timeout path (`:479`), `TerminateJobObject` runs first. Once the root is dead,
`taskkill /T /PID <dead root>` finds no process and kills nothing. Because job termination is
asynchronous, a short race remains. These paths are rare: timeout, Ctrl+C, or an unexpected
throw. That is why this is LOW and not MEDIUM.

**Is `/T` on the root safe?** Not fully. It is safe only once the root is gone.

**Fix.** Replace `Stop-Tree` with a job-membership kill. Call
`QueryInformationJobObject(JobObjectBasicProcessIdList = 3)` and terminate every member except
`SpareNames`, through a handle opened per PID. Job membership is a property of the process
object, so PID reuse cannot add a stranger. Where no job exists, walk with the same
`Test-ChildAdoptable` rule and kill without `/T`.

### LOW-2: build children spawned in the last scan window can now escape reaping

These lines are involved:

- `tools/negtest.ps1:405`: `if (-not $byId.ContainsKey($rootId)) { return }`
- `:423`: the kill no longer uses `/T`
- `:381`: `Stop-CopyProcesses` no longer uses `/T`

**Scenario.** The tracker scans every 1.5 s, and the first scan comes about 0.5 s after the start.
Suppose a descendant is spawned and its parent exits between two scans. That descendant is never
added to `$tracked`. The final `Add-Descendants` (`:481`) runs after the root has exited, so
`:405` turns it into a no-op.

In practice the old final scan was nearly useless as well: the root's only child, the wrapper
`powershell`, has always exited by then. Before this change, `taskkill /T` on a tracked parent
or on a command-line match also killed that parent's untracked children. Now nothing does. The
only remaining backstop is the command-line match in `Stop-CopyProcesses`.

A child whose command line does not contain the copy path, but whose cwd is inside the copy,
survives. Examples are `node tests\x.js` or `cmd /c` with relative paths. When that happens,
`Remove-Copy` fails loudly (`COPY NOT REMOVED`), the process leaks, and the directory is only
swept later. The run's job is disarmed on a normal exit, so it does not catch the child either.

**Fix.** Use the same job-member enumeration as in LOW-1, on the normal-exit path as well:
before disarming, kill every member except `SpareNames`. Also pass `$p.StartTime` as the root's
creation time, so the final scan still walks the root's direct children after the root exits.
The root PID cannot be reused while `$p.Handle` is held.

### LOW-3: the new code is mostly untested; mutations survive

`tools/check_negtest.ps1:258-267` and `:418-426`; `tools/check_push_verify.ps1`.

- **L6 has no test.** Neither a tracked duplicate nor an untracked duplicate is exercised.
  `check name resolution` covers only a unique name and a missing name. As a result,
  L6_include_untracked and L6_no_ambiguity were both MISSED. Fix: in the scratch repo, add an
  untracked `logs/x/check_fixture.ps1` and assert that the name still resolves. Then add a
  second tracked copy and assert exit 2 with `ambiguous`.
- **The adoption test only checks the source text.** `:266` asserts that the string
  `Test-ChildAdoptable $c $parentCreated` appears. Any change that keeps the string but defeats
  the call passes, which is how adopt_bypass_keeps_string was MISSED. Fix: extract
  `Add-Descendants` as well and feed it a fake process list. Include a stale-PPID process older
  than its parent, and assert that it is not tracked.
- **`Stop-CopyProcesses` has no `/T` guard.** `:267` guards only the `$k` form in
  `Stop-Tracked`. A `/T` reintroduced into `Stop-CopyProcesses` (copyproc_tree_kill) is MISSED.
  The same check also misses argument reordering such as `/F /T`. Fix: assert that no
  `taskkill` call anywhere in the file uses `/T`, apart from `Stop-Tree` until LOW-1 is fixed.
- **The push_verify retry is untested** (`pv_retry_once` and `pv_no_break` were MISSED). This is
  acceptable, because the retry only affects temp-file cleanup, never the verdict. If a test is
  wanted, assert that no `tmp*.tmp` file is left behind after the failed-fetch case.

### LOW-4: the audit docs cite pre-rebase SHAs

`docs/audits/REVIEW_FIREFX3_TOOLFX2_2026-10-10.md` (B-MEDIUM-1 and B-LOW fix lines) cites
`46afbf427`. `docs/audits/REVIEW_R2ACE_TOOLFX4_2026-10-10.md` (L6) cites `4bab578a4`. Both objects
exist locally, but no remote branch contains them. The commits that actually landed are
`93df083f2` and `44ecaf482`. Fix: correct the two citations.

### INFO

- **`-RequireAssertion` CAUGHT does not require a nonzero exit** (`tools/negtest.ps1:559`). Suppose
  a mutated run exits 0 but prints a non-gating `  FAIL x.c:1: ...` line, for example from an
  informational sim. That run counts as CAUGHT, even though the check it guards would still pass.
  Fix: under `-RequireAssertion`, require both `$r.Exit -ne 0` and a matching line. The baseline
  gate (`:532`) already rejects a pattern that is present in the unmutated output, so the
  matcher can never report every mutation CAUGHT.
- **Other false-CAUGHT risks from the matcher.**
  - Matching runs per line (`Get-MatchLines` splits on newlines), so `^\s+` cannot span a blank
    line.
  - These outputs do not match `  FAIL \S+:\d+:` or `\bFAIL:`, as confirmed from the regex and
    the `reqassert_buildfail` case:
    - MSVC diagnostics (`file.c(12): error/warning`)
    - GCC/clang diagnostics (`file.c:12:5: error`)
    - ninja `FAILED:`
    - `BUILD FAILED:`
  - The remaining risk is a harness that echoes fixture text. For example, a failing
    `check_negtest` group prints `$r.Text`, which contains `  FAIL calc.c:42:`. The `\bFAIL:`
    alternative already carried this risk before the change.
  - The opposite error is possible too: a `__FILE__` path containing a space breaks `\S+` and
    produces a false MISSED. No current `-CopyRoot` contains a space.
- **`Stop-Tracked` has a check-then-kill race** (`:422-423`). The PID could be reused in the
  milliseconds between the `CreationDate` query and `taskkill /F /PID`. Fix: open a handle once,
  using `Get-Process -Id`, compare its `StartTime`, then `Kill()` that same object.
- **`Stop-CopyProcesses` now spares `ccache.exe`** (`:381-382`). Of the four spared names, only
  `ccache.exe` can have the copy path on its command line: `mspdbsrv`, `vctip` and `conhost` never
  do. A matching `ccache.exe` is a per-compile wrapper inside the copy, not a shared daemon, so
  sparing it can only delay copy removal. Fix: in `Stop-CopyProcesses`, spare only
  `mspdbsrv.exe` and `vctip.exe`.
- **The test's own cleanup uses `/T`.** `tools/check_negtest.ps1:282` kills surviving
  token-matched processes with `/T`, so the LOW-1 risk is confined to the test's own cleanup.

### Verified correct

- **`Test-ChildAdoptable`** (`tools/negtest.ps1:394-397`).
  - A stale PPID always belongs to a process created before the PID's current holder. Its real
    parent had to die before the PID could be reused, so `child < parent` is strict, and
    microsecond resolution cannot collapse it into `-ge`.
  - "Reparenting to an existing PID" is that same case.
  - A null `CreationDate` on either side fails safe: the process is not adopted and not walked.
  - A backward clock step can only cause a missed adoption (a leak), never a wrong kill.
  - Each edge is checked separately, so the walk no longer descends through a stale node
    (B-MED-1 case (b)).
- **`Stop-Tracked` identity check** (`:423`): comparing the stored `CreationDate` string rejects a
  reused PID. A DST change between tracking and killing could make the strings differ, which
  again causes a leak, not a wrong kill.
- **L6** (`:668-670`).
  - Default git pathspec `*` matches across `/`.
  - Index entries that are missing on disk are dropped by `Get-Item`.
  - A git failure gives `no such check` (fail closed).
  - Submodule checks are no longer found (`ls-files` does not recurse). No current submodule
    holds a `check_*.ps1` that `run_all_checks` discovers.
- **push_verify retry** (`tools/push_verify.ps1:114-117`).
  - `break` inside `try` exits the `for` loop.
  - `File.Delete` on a missing file does not throw.
  - The retry is at most 5 x 300 ms.
  - The result never feeds `$fetchOk` or the verdict.
