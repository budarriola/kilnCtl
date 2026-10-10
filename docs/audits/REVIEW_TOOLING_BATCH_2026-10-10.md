# Review: tooling batch 2ac609343 (2026-10-10)

Scope: the single commit `2ac609343` on origin/dev ("Tooling batch: negtest
default verdict patterns and -RequireAssertion, push_verify default HEAD plus
bounded fetch, dev-tip main baselines, legacy stack checks flag dropped
long-call literals INDETERMINATE"), parent `a9c3d5b08`. Review only. Nothing
was fixed.

Focus: any path that can now report a false green.

Method:
- read the diff;
- ran `tools/check_negtest.ps1`, `tools/check_push_verify.ps1` and
  `tools/check_main_baseline.ps1` as-is;
- negative-tested each of them with `tools/negtest.ps1 -Preset check` (the
  mutations ran in throwaway worktrees);
- ran scenario scripts against throwaway git repos for baseline selection and
  push_verify. Those used a scratch copy of `main_baseline_lib.ps1` with the
  quoting bug below fixed.

## Summary

| ID | Severity | Area | Finding |
|----|----------|------|---------|
| B1 | CRITICAL | check_main_baseline / main_baseline_lib | An unquoted `%(objectname)` throws inside `Select-MainBaseline`. `check_main_baseline.ps1` still prints "all cases passed" and exits 0 after running only 24 of its ~75 assertions (69 ran before this batch). The dev-tip feature is dead code, and the check is vacuous. |
| B2 | HIGH (latent, activates once B1 is fixed) | main_baseline_lib Exact rule | With the quoting fixed, two scenarios mark a NEW failure KNOWN. |
| P1 | MED | push_verify | `git fetch` exits 0 without updating `refs/remotes/origin/dev` when the remote fetch refspec does not cover the branch. push_verify then reports LANDED off a stale ref. The bounded-fetch change does not address this. |
| P2 | MED | push_verify | `-Commit` now defaults to `HEAD`. An agent that forgot to commit, or ran in the wrong tree, gets LANDED for the base commit. |
| P3 | LOW | push_verify | A negative `-FetchTimeoutSec` makes `WaitForExit` throw. The result falls through to UNKNOWN with an empty "exit" message and may leave git running. Temp stdout/stderr files are never deleted. A failed fetch has no test (negtest MISSED). |
| N1 | MED | negtest saftyfw-host preset | `BUILD FAILED` counts as CAUGHT, so a compile or `/W4 /WX` break (for example, an unused variable left by deleting a guard) counts as a test catch. kilnfw-host deliberately does not count a build failure. The two presets are inconsistent. |
| N2 | MED (latent) | negtest -RequireAssertion | The assertion regex accepts `^not ok`. node:test's TAP reporter prints `not ok` for a crash such as a ReferenceError, so a crash counts as CAUGHT. `(?i)` makes `\bFAIL:` match `fail:` in ordinary log text. The switch is silently ignored when `-ExpectPattern` or a preset pattern is in effect. |
| N3 | LOW | check_negtest | The preset assertions are source-text `Contains` checks, not behavioral checks. The pytest default pattern is untested (negtest MISSED). |
| S1 | LOW / INFO | legacy stack budget checks | INDETERMINATE does not hide an overflow beyond the previous behavior. It never gates, though, and it fires on essentially every real ELF (183 functions for the executor check), so the line is noise. Its absence does not mean the depth is complete. |

## B1 (CRITICAL): unquoted `%(objectname)`, vacuous baseline check

`tools/main_baseline_lib.ps1`, `Select-MainBaseline`, around line 232:

```powershell
if ($DevRef -and (& git -C $RepoRoot for-each-ref --format=%(objectname) "refs/remotes/$DevRef" 2>$null | Out-String).Trim())
```

In PowerShell argument mode, `%(objectname)` is parsed as `%` (an alias for
ForEach-Object) followed by a subexpression. That evaluates `objectname` as a
command and throws `The term 'objectname' is not recognized`. Every call to
`Select-MainBaseline` with a dev ref configured (the default) therefore throws
before it selects anything.

Consequences:

- **`tools/check_main_baseline.ps1`** runs under
  `$ErrorActionPreference = "Continue"` inside a `try`/`finally` with no
  `catch`. The throw from the first `Select-MainBaseline` call (line ~154,
  lineage case) ends the `try` block. The script then prints "all cases
  passed" and exits 0.
  - Only 24 assertions run. The pre-batch version ran 69, which I verified by
    extracting `a9c3d5b08`'s copy and running it.
  - The new dev-tip case never executes.
  - Negative tests: `mb_control_known` (a mutation in code reached before the
    throw) was CAUGHT. `mb_drop_mbdev_exact`, `mb_dev_ref_label`, and
    `mb_dev_tree_exact_always` (`-or $true`, which makes every baseline Exact
    and so turns every failure KNOWN) were all MISSED.
- **`tools/run_all_checks.ps1`** (`$ErrorActionPreference = "Stop"`):
  `Exit-WithBaseline` catches the throw and prints
  `main baseline: skipped (...)`.
  - No baseline comparison runs, and no baseline is recorded on either main or
    dev.
  - This fails closed: `-FailOnlyOnNew` keeps the failing exit code. But the
    whole NEW/KNOWN feature is silently off for everyone.
- **`tools/land.ps1`** (line ~262) calls `Select-MainBaseline` under Continue.
  `$sel` stays null, so the result is "no usable baseline" and
  `-AllowKnownFailures` refuses. This also fails closed.

The false green belongs to the check, not to the landing path. Today the
landing path fails closed only by accident. Fixing just the quoting turns on
B2.

Separately, the check's structure (Continue plus `try`/`finally`, no assertion
count) lets any exception inside it pass silently. The check should fail on an
uncaught error, or assert how many cases ran.

## B2 (HIGH, latent): Exact rule marks NEW failures KNOWN

The Exact rule (around line 256) is:

```powershell
$isExact = ($mb -and $b.commit -ceq $mb) -or (mainTree match) -or ($mbDev -and $b.commit -ceq $mbDev) -or (devTree match)
```

Selection takes the **first** Exact baseline in file (tree-hash filename)
order. It does not take the closest or newest one.

The scenarios ran against a throwaway repo with the following history:

- main: `R` - `F` - `P` (promote squash of dev) - `M2`
- dev: forked at `F`, then `D1` - `D2`

A scratch copy of the lib with the quoting fixed was used.

- **(A) Main-lineage HEAD, baseline only at the dev fork point `F`, with
  check_x failing there.**
  - HEAD = `M2` + one commit. `mbDev` = merge-base(HEAD, origin/dev) = `F`.
  - The `F` baseline becomes Exact via the new `mbDev` clause, so a check_x
    failure at HEAD is KNOWN.
  - `F` is a stale main commit from before `P` and `M2`, and the baseline
    should at best be a warned, non-exact fallback.
  - This matters in the real repo: merge-base(origin/main, origin/dev) is
    `676c503f8`, a main commit.
- **(B) Dev-lineage HEAD, baselines at both `F` (check_x FAIL) and `D2` (check_x
  PASS).**
  - Both are Exact. The `F` one is Exact via `mb`, a rule that predates this
    batch; on dev, `mb` is always the fork point.
  - First-Exact-by-filename picked `F`, so the failure is reported KNOWN even
    though the check passed at the dev tip `D2`.
  - Which baseline wins depends on hash ordering, so the result is not
    deterministic across histories.

Underlying issue, which predates this batch but is widened by it: on a dev
lineage, a fork-point main baseline is Exact for every dev commit, however far
dev has moved.

Recommendations (not applied):
- prefer the most specific or newest Exact baseline, with the dev tip before
  the fork point;
- do not let `mbDev` qualify for a HEAD whose lineage is main;
- add check cases for (A) and (B) once B1 is fixed.

## P1 (MED): push_verify LANDED on a stale remote-tracking ref

push_verify runs `git fetch` (now bounded) and then tests ancestry against
`refs/remotes/origin/<branch>`.

If the remote's configured fetch refspec does not cover that branch (for
example, a narrowed `remote.origin.fetch`, or a single-branch clone),
`git fetch` exits 0 and the remote-tracking ref stays stale.

Reproduced in a throwaway repo:
1. Commit X was on dev.
2. Remote dev was then rewritten so X was no longer on it.
3. The fetch refspec covered only main.
4. push_verify reported LANDED for X.

This behavior predates the batch, but the batch's "bounded fetch" claim
implies the ref is fresh, and it is not checked. Suggestion: fetch with an
explicit refspec (`+refs/heads/<b>:refs/remotes/origin/<b>`), or cross-check
the result against `git ls-remote`.

## P2 (MED): `-Commit` defaults to `HEAD`

`[string]$Commit = "HEAD"`.

An agent that forgot to commit, or whose commit failed (for example, a hook
refusal), still has HEAD at the base commit. That commit is already on
origin/dev, so push_verify reports LANDED. The same happens when it is run
from the wrong worktree.

The old mandatory parameter forced the caller to name the SHA they meant.
check_push_verify covers the default itself (negtest `pv_default_head_tilde`
CAUGHT), but nothing guards this misuse.

Suggestion: keep the default, but print the resolved SHA and subject
prominently and warn when HEAD equals the remote tip (nothing new pushed).

## P3 (LOW): push_verify edge cases

- A negative `-FetchTimeoutSec` makes `WaitForExit` throw. The outcome is
  UNKNOWN (fail-closed) with an empty "exit" value in the message, and the git
  child process may be left running.
- The temp stdout/stderr files from `Start-Process` are never removed.
- check_push_verify has no failed-fetch case: negtest `pv_ignore_fetch_failure`
  was MISSED.

## N1 (MED): saftyfw-host preset counts BUILD FAILED as CAUGHT

The preset patterns are:
- kilnfw-host: `RUN FAILURES \(`
- saftyfw-host: `SAFTYFW HOST TESTS: (FAILED|BUILD FAILED)`
- pytest: `(?m)^FAILED \S+`

`firmware/SaftyFW/test/build_host_tests.ps1` prints
`SAFTYFW HOST TESTS: BUILD FAILED` (line ~646) and builds with `/W4 /WX`.
Deleting a guard statement that leaves a variable unused breaks the build, and
negtest reports CAUGHT without any test having run.

The KilnFW script prints `BUILD FAILURES (` for a compile break, which the
kilnfw preset does not match, so a compile-breaking mutation there is MISSED.
That is the honest direction.

One more gap: a SaftyFW script throw before the verdict line (line ~649)
prints no verdict, so the result is MISSED, which is safe.

Suggestion: make saftyfw-host match only the test-failure verdict, in line
with kilnfw-host, and report a build break as its own non-CAUGHT outcome.

## N2 (MED, latent): -RequireAssertion accepts crashes

The pattern (negtest.ps1 line ~626) is:

```text
(?im)(AssertionError|ERR_ASSERTION|assertion failed|^not ok|\bFAIL:)
```

Problems:
- `^not ok`: node:test uses the TAP reporter by default when stdout is not a
  terminal, and it prints `not ok` for any failing test, including one that
  died with a ReferenceError. Verified with node v24.18.0. A crash therefore
  counts as an assertion, which is exactly what the switch exists to reject.
  The repo does not use node:test today, so this is latent.
- `(?i)` together with `\bFAIL:` matches ordinary log text such as
  `... fail: retrying`.
- `-RequireAssertion` is silently ignored whenever `-ExpectPattern` is passed
  or a preset supplies its own pattern. A caller who asked for it gets no
  warning that it had no effect.

## N3 (LOW): check_negtest coverage

- The preset-pattern assertions in check_negtest are source-text `Contains`
  checks, not behavioral checks.
- The pytest default pattern has no behavioral test: negtest
  `ng_pytest_error_counts` was MISSED.
- `ng_reqassert_off`, `ng_reqassert_loose` and `ng_kilnfw_buildfail_counts`
  were CAUGHT.

## S1 (LOW / INFO): INDETERMINATE in the legacy stack checks

`check_main/executor/httpd_task_stack_budget.py`: `parse_ex` now returns the
long-call `l32r` literals it dropped, and `indeterminate_note` prints
INDETERMINATE without changing the exit code.

- This does not hide an overflow beyond the old behavior. The depth was always
  a lower bound, and the exit code is unchanged.
- It never gates. A run that prints INDETERMINATE is still PASS and is stored
  in the check result cache.
- Against the main tree's (stale, 2026-09-25) ELF, the executor check flagged
  183 reachable functions. The line will appear on essentially every run and
  be ignored.
- It covers only `l32r`-loaded call targets. Indirect `callx` through
  struct or vtable function pointers stays silent, so the absence of
  INDETERMINATE does not mean the call graph is complete.
- `test_stack_budget_symbol_bounds.py`: 22 passed.

## Fix status (2026-10-10)

B1, B2, P1, P2, P3, N1, N2 and N3 are FIXED in `1d54d0730`, with test follow-ups
`e5936650a` (B2b rank ordering made load-bearing), `c43e0a0f2` (P1 test narrows the
refspec to an existing branch) and `3d9486bbf` (N2 not-ok fixture effective).

- B1: `--format=%(objectname)` quoted; check_main_baseline fails on any uncaught
  error and asserts its count (81).
- B2: a fork-point baseline is no longer Exact via mbDev for a main-side HEAD; Exact
  baselines are ranked (HEAD-tree match, then dev-tree, then other), then commit time,
  then commit id.
- P1: explicit refspec fetch. P2: `-Commit` required (exit 2). P3: `-FetchTimeoutSec`
  validated, git process tree killed on timeout, temp files removed, failed-fetch test.
- N1: saftyfw-host preset counts only `SAFTYFW HOST TESTS: FAILED`. N2: `-RequireAssertion`
  is case-sensitive, drops `^not ok`, and is refused with `-ExpectPattern` or a preset.
  N3: behavioural test of the pytest default pattern.
- Extra: negtest now tracks and kills worker descendants (host_build_worker, host-test
  exes) on every exit path; test `orphan_reaped`.
- Also fixed `check_submodule_pins_pushed.ps1`: `$PSScriptRoot` param default is empty
  under 5.1 `-File`; resolved in the body.
- S1 not addressed.

Re-run negative tests: mb_unquote_format, mb_drop_b2a, mb_rank_off,
mb_dev_tree_exact_always, mb_dev_ref_label, pv_no_refspec, pv_ignore_fetch_failure,
ng_pytest_error_counts, ng_saftyfw_buildfail_counts, ng_reqassert_notok,
ng_reqassert_ci, ng_reqassert_ignored, ng_no_tracked_kill: all CAUGHT. Unmutated:
check_main_baseline PASS (81 assertions), check_push_verify PASS, check_negtest PASS
(185 assertions).

## Negative-test results

All runs used `tools/negtest.ps1 -Preset check -PresetArg <check> -Mutations <json>`.

| Check | Mutation | Result |
|-------|----------|--------|
| check_main_baseline | mb_control_known (control, code before the throw) | CAUGHT |
| check_main_baseline | mb_drop_mbdev_exact | MISSED (B1) |
| check_main_baseline | mb_dev_ref_label | MISSED (B1) |
| check_main_baseline | mb_dev_tree_exact_always (`-or $true`) | MISSED (B1) |
| check_push_verify | pv_default_head_tilde | CAUGHT |
| check_push_verify | pv_no_timeout | CAUGHT |
| check_push_verify | pv_reverse_ancestry | CAUGHT |
| check_push_verify | pv_ignore_fetch_failure | MISSED (P3) |
| check_negtest | ng_reqassert_off | CAUGHT |
| check_negtest | ng_reqassert_loose | CAUGHT |
| check_negtest | ng_kilnfw_buildfail_counts | CAUGHT |
| check_negtest | ng_pytest_error_counts | MISSED (N3) |

Unmutated runs: check_push_verify PASS; check_negtest PASS (163 assertions);
check_main_baseline PASS, but vacuously (24 assertions, B1).

## Recommended order

1. B1, together with B2. Fixing the quoting alone enables the KNOWN
   misclassification. Make check_main_baseline fail on an uncaught error and
   assert its case count.
2. P1 and P2 (push_verify freshness and the default-HEAD warning).
3. N1 and N2 (negtest verdict precision).
4. P3, N3 and S1 as convenient.
