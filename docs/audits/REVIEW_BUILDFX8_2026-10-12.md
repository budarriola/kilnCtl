# Review: buildfx8 (buildlock REVIEW_BUILDFX7 fixes), 2026-10-12

Adversarial review of three commits on origin/dev:

- `dcce7097a` buildlock fixes and tests
- `a139510d4` M9 test change
- `12b36c8ae` review doc update

These close REVIEW_BUILDFX7 LOW-1 and LOW-2, plus INFO-1..5, in
`tools/PcTools/src/mcpkit/buildlock.py` and `tools/PcTools/tests/test_buildlock.py`.
The review was done at origin/dev `859bf2a9a` in worktree `C:\wt\rvbuildfx8_35cu0c`.
Nothing was changed except this document.

**Verdict:** 0 HIGH, 0 MED, 3 LOW, 6 INFO.

Mutual exclusion holds in every case examined. The new `_put_back` never
overwrites another holder's lock. The `_maybe_leaked` free path cannot free a
lock that another process or a different acquire holds. The findings are an
unbounded wait on the new free path, a sweep that judges an aside's age by the
wrong timestamp, and one test assertion that was removed without cause.

## Focus answers

### 1. `_put_back` retry and identical-text delete

`_put_back` (`buildlock.py:236-260`) cannot overwrite a live lock on Windows.

- On Windows, `os.rename` is MoveFileExW without MOVEFILE_REPLACE_EXISTING, so
  an existing destination raises FileExistsError. That path never retries.
- The retried errors are PermissionError (a sharing violation, or a destination
  pending delete) and other OSErrors. None of these replaces the destination.
- The 400 x 5 ms retry is strictly better than buildfx7, which deleted the
  aside on any OSError.

The identical-text delete is effectively only the `None == None` case (repro
R3). Lock text carries a per-acquire uuid4 token, so two different live
locks never have equal non-None text. When both the aside and the newcomer read
as None (empty or unreadable), the aside is deleted. That aside's owner has
already lost the path to the newcomer, so mutual exclusion is unaffected
(INFO-1).

On the second half of the question: a leaked aside is eventually swept, because
`_sweep_asides` runs at every `build_lock` entry over the whole directory. The
sweep never touches a real `*.lock` file or a `.reclaim` sidecar, since its
regex requires a `.(rec|rel|dead).<32 hex>` suffix. However, it judges age by
the file's mtime, and `os.rename` preserves the mtime of the lock being moved.
So it can delete an aside that is in flight, which is a live lock mid-restore
(LOW-2).

### 2. `_maybe_leaked` and freeing on the next contention

An exact text match cannot free another process's lock.

- The key is `(lock_path, "<pid> <start> <uuid4 hex>")`, and the token is fresh
  for every acquire.
- PID reuse alone does not collide. A reused PID with the same start time and a
  different token is not freed (repro R5).
- A collision needs a uuid4 collision.
- Across threads in one process, the check-then-discard is benign. A second
  thread that also matches would call `_release` on a file the first thread
  already removed, and that call returns on FileNotFoundError.

The free path does have a timeout defect (LOW-1). Other processes still wait
until this process contends on that key again or restarts (INFO-2).

### 3. Weakened test assertion

`a139510d4` removed the "no `.rel.` aside remains" assertion from two tests and
added cleanup of `.rel.` files to both.

- `test_release_restore_never_overwrites_a_newer_lock` (line 330): the removal
  is justified. A newer lock owns the path, so `_put_back` leaves a differing
  aside on purpose. A positive assertion would be better here: the aside exists
  and still holds the grabbed lock's text.
- `test_release_puts_back_a_lock_it_does_not_own` (line 460): the removal is
  not justified. The path is vacant after the move-aside, so the put-back
  succeeds and no aside should remain. The new comment ("A differing grabbed
  file is left as an aside") describes the other test's case, not this one.
  Mutation N8 makes the release recreate the lock instead of moving the aside
  back, which leaves a duplicate `.rel.` file. N8 passes all eight
  `BuildLockReviewFixes` tests: **MISSED**. This is LOW-3.

### 4. How buildfx8 landed

The land went through `land.ps1 -Target dev` without `-CheckLog`. That is
allowed under the dev flow, so it did not bypass a mandatory gate. Sources:
`C:\wt\buildfx8_land.log`, `C:\wt\buildfx8_checks.log`, the reflog of
`C:\wt\buildfx8_ps15w4`, and `git range-diff`.

**Land log steps:**

1. Preflight.
2. Rebase onto origin/dev (attempt 1).
3. Narrow post-rebase checks (`check_mcp_tool_count_doc|check_mcp_facade_coverage`): 2 passed.
4. Submodule pins.
5. Push.
6. push_verify LANDED `12b36c8ae`.

There is no "check-log gate" step, and the final JSON has `allowed_fails`,
`known_fails` and `new_fails` all empty. `land.ps1` requires `-Coordinator`
and `-CheckLog` only for `-Target main`. This matches the 2026-10-09 flow:
agents push to dev, and the coordinator full-runs the dev tip.

**The -Fast run** (`buildfx8_checks.log`, 11:04) ran on the pre-rebase tip
`b205d71fa`, clean tree, mode fast. Result: 177 passed,
4 failed ("4 of 198 checks FAILED"). The run's baseline was not at the
merge-base, so all four counted as NEW. None involves buildlock:

| Check | Failure | Later fix |
|---|---|---|
| `check_source_path_drift` | `tools/check_host_test_verdict.ps1:31/171` | `eb10d0caa` |
| `check_doc_hash_citations` | a non-resolving hash cited at `REVIEW_LCDFX13_2026-10-12.md:108` | `243f90f00` |
| `test_host_fakes.ps1` | 900 s timeout | none recorded |
| `check_land.ps1` | 900 s timeout | none recorded |

The log was never passed to land, so the failures did not block it. Under the
dev flow, this matches the rules.

**Reflog:** commits `8a190fa20`, `99553f806` and `b205d71fa` on base
`83e9555ab` were rebased at 11:25 onto `d5fadaed4`, becoming `dcce7097a`,
`a139510d4` and `12b36c8ae`. `git range-diff` shows all three identical (`=`).

**Fixer negtests:** `buildfx8_neg.log` caught all 12 mutations (M4, M5, M8, M9,
I1, L1a, L1b, L2a, L2b, I2a, I2b, I3), and `buildfx8_neg2.log` caught M9.

`run_all_checks.ps1` has no check that runs `tools/PcTools/tests/test_buildlock.py`.
So neither the -Fast run nor the coordinator's full dev-tip run tests buildlock
at all; only the fixer's own pytest and negtests do (INFO-4).

## Findings

### LOW-1: freeing a leaked lock skips `wait_timeout`, so the wait is unbounded

`build_lock` contention branch, `buildlock.py:465-469`:

```python
if judged is not None and (lock_path, judged) in _maybe_leaked:
    _maybe_leaked.discard((lock_path, judged))
    _release(lock_path, judged)
    continue
```

The `continue` skips the `waited >= wait_timeout` check below it. If `_release`
cannot rename the lock aside (a persistent sharing violation from AV or an
indexer), it logs `LEAKED LOCK` and re-adds the same `(path, text)` entry.
The next pass matches again, and the loop never times out.

**Repro R1:** `wait_timeout=0.3`, the lock is in `_maybe_leaked`, and
`os.rename` always raises PermissionError. After 10 s the waiter thread was
still alive, after 1121 rename attempts, and never raised BuildLockTimeout. It
acquired as soon as the rename was unblocked.

Each pass costs about 2 s inside `_release`, so this is a slow loop rather than
a hot spin. The `# No continue: fall through so the timeout is honoured`
comment two lines below states the rule this branch breaks.

**Fix:** after `_release`, check the timeout before `continue`, or fall through
to the shared timeout and sleep path.

### LOW-2: `_sweep_asides` judges age by the lock's mtime, not by when it was moved aside

`os.rename` keeps the mtime of the file it moves. A `.rel.` or `.rec.` aside of
a lock that has been held for more than `stale_after` (2100 s in production)
is therefore eligible for the sweep the instant it is created. Another
process's `build_lock` entry sweeps the whole directory, and it can delete the
aside while its owner is in the middle of `_put_back`.

**Repro R2 (live foreign lock lost):** B holds a live lock with an mtime 3000 s
old. A, which was itself reclaimed earlier, runs `_release` and moves B's lock
aside on a token mismatch. A concurrent `_sweep_asides(dir, 2100)` deletes the
aside before the put-back. Afterwards the path is vacant, no files remain, and
stderr shows `could NOT restore ... may be lost`. B now holds nothing on disk,
and a third builder can acquire.

The precondition is that A was reclaimed while alive. That is the same
precondition as the residual windows already documented in `_release`'s
docstring, so this is rare. It is new loss surface introduced by the sweep,
though.

**Repro R2b (false alarm on own lock):** this process holds its own lock for
more than `stale_after`. A concurrent sweep during `_release` deletes the
aside, and a false `could NOT restore it ... may be lost` is logged. The lock
is correctly gone, and nothing is added to `_maybe_leaked`.

**Fix:** time the aside's age from the move-aside, not the lock's mtime. For
example, put the move-aside epoch in the aside name, or `os.utime` the aside
immediately after the rename (this still leaves a small window), and sweep on
that time.

### LOW-3: removed assertion hides a duplicate-aside regression

See focus 3. In `test_release_puts_back_a_lock_it_does_not_own`, restore the
check that no `<lock>.rel.*` file remains. In
`test_release_restore_never_overwrites_a_newer_lock`, add a positive check that
exactly one aside exists and holds the grabbed lock's text. N8 (MISSED) is the
proof that the current tests miss this.

### INFO-1: `_put_back`'s identical-text delete only fires for `None == None`

See focus 1 and repro R3. This is harmless, but the docstring implies a real
duplicate check. Either state the None case explicitly, or require
`_read_holder(aside) is not None` before deleting, so an unreadable aside is
kept for the sweep instead.

### INFO-2: the leak set lives only in this process

A leaked lock is freed only when this process contends on the same key again.
Other servers wait until it does, or until the lock passes `stale_after`. That
is acceptable for a single MCP server, but the `LEAKED LOCK -- could not rename`
message (`buildlock.py:503`) still says builds "will wait until the server
restarts or the file is deleted". The message should also mention the
next-contention free.

### INFO-3: tests sweep the shared production lock directory

`BuildLockBuildFx8` tests call `build_lock` and `_sweep_asides` with
`stale_after=60`, against the real `%TEMP%\kilnctl-builds\locks`. A test run
can therefore sweep a production `.rel.` or `.rec.` aside older than 60 s,
including LOW-2's in-flight case. Point these tests at a temporary directory.

### INFO-4: `run_all_checks.ps1` does not run `test_buildlock.py`

No `check_*` covers `tools/PcTools/tests/test_buildlock.py`. A green -Fast or
full run says nothing about buildlock regressions.

### INFO-5: the full-file pytest preset flakes under load

A full-file run of `test_buildlock.py` under negtest took 206 s at baseline.
`BuildLockSerializesConcurrentProcesses::test_two_processes_serialize_and_both_results_are_correct`
then failed `assertFalse(p1.is_alive())` from a timeout, not from the
mutation. That produced a spurious CAUGHT for N8, which was discarded.
Negtests of buildlock should target the specific class with
`-PresetArg tests/test_buildlock.py::<Class>`.

### INFO-6: the land used the dev flow

See focus 4. The land is recorded here and was not changed.

## Repros

The repro script is the scratch file `repro_fx8.py`. It runs against
`859bf2a9a` with `PYTHONPATH=tools/PcTools/src`.

| Id | Scenario | Result |
|---|---|---|
| R1 | Lock in leak set, rename always PermissionError, `wait_timeout=0.3` | Still waiting after 10 s and 1121 rename attempts; no timeout (LOW-1) |
| R2 | Live foreign lock with mtime 3000 s old, mismatch release concurrent with `_sweep_asides(2100)` | Aside swept, path vacant, foreign lock lost (LOW-2) |
| R2b | Own lock held longer than `stale_after`, concurrent sweep during release | False "could NOT restore" log; lock correctly gone |
| R3 | `_put_back` with aside and newcomer both unreadable | Aside deleted (`None == None`) (INFO-1) |
| R4 | `_put_back` with a differing newcomer | Aside kept; path stays the newcomer's |
| R5 | Same pid and start time, different token, in leak set | Not freed; BuildLockTimeout |

## Negtests (this review)

All runs used `tools\negtest.ps1 -Preset pytest`. Every run reported baseline
PASS, `real_tree_unchanged: true` and `copies_removed: true`.

- N1-N10 ran on the full file, log directory `%TEMP%\negtest_logs\20261012_113541_43pv`.
  Each CAUGHT run was 1 failed, 57 passed.
- The N8 rerun was scoped to `tests/test_buildlock.py::BuildLockReviewFixes`,
  log directory `%TEMP%\negtest_logs\20261012_121459_ge0y`.

| Id | Mutation | Verdict | Evidence |
|---|---|---|---|
| N1 | `_put_back` always deletes the aside on conflict | CAUGHT | `test_put_back_deletes_only_a_matching_duplicate`: AssertionError "different text must not be deleted" |
| N2 | `_put_back` never deletes a duplicate | CAUGHT | same test: AssertionError "True is not false" |
| N3 | `_put_back` does not retry transient errors | CAUGHT | `test_put_back_retries_transient_errors_and_keeps_the_lock`: AssertionError "None != '1 0 H'" |
| N4 | Leak-set free path disabled | CAUGHT (exception) | `test_leaked_own_lock_is_freed_on_next_contention` errors with BuildLockTimeout "waited 5.0s"; a behavioral failure, not a compile or import error, but not an assertion |
| N5 | Free path frees any lock on that path | CAUGHT | `test_foreign_lock_is_not_freed_by_the_leak_set`: AssertionError "BuildLockTimeout not raised" |
| N6 | `judged is None` mtime guard removed | CAUGHT | `test_fresh_empty_lock_is_not_reclaimed_by_judged_none`: AssertionError |
| N7 | Sweep ignores age | CAUGHT | `test_acquire_sweeps_old_asides_only`: AssertionError |
| N8 | Mismatch release recreates the lock, leaving a duplicate `.rel.` aside | **MISSED** | 8 of 8 `BuildLockReviewFixes` tests pass (LOW-3). The first attempt was void (SyntaxError from an escaped `\n`). The full-file rerun's CAUGHT came from an unrelated timeout flake (INFO-5) and was discarded. A killed rerun was also void. |
| N9 | Held-None branch never records the leak | CAUGHT | `test_leaked_own_lock_is_freed_on_next_contention`: AssertionError on the `(path, text)` membership check |
| N10 | "restored it to" printed even when the put-back fails | CAUGHT | `test_restored_logged_only_after_put_back_succeeds`: AssertionError "'restored it to' unexpectedly found" |

Totals: 8 CAUGHT by assertion, 1 CAUGHT by exception (N4), 1 MISSED (N8).

## Fix status (buildfx9)

Fixed on origin/dev in 21118b47b (fixes and tests), 562655f33 and f74e6cbdb (LOW-1 test bounded so a regression fails instead of hanging).
`tools/PcTools/src/mcpkit/buildlock.py`.

- LOW-1 fixed: the leaked-lock free path checks `wait_timeout` before `continue`.
- LOW-2 fixed: `_touch_aside` re-stamps the aside's mtime at move time, so the sweep ages it from the move; `_remove_judged` reads the lock's mtime before the re-stamp.
- LOW-3 fixed: the no-`.rel.`-aside assertion is back; the newer-lock test asserts the aside exists with the grabbed text. N8 is now CAUGHT.
- INFO-2 fixed: LEAKED LOCK message states the next-contention free.
- INFO-3 fixed: `BuildLockBuildFx8` uses a temporary lock directory.
- INFO-4 not fixed: no standard run_all_checks wiring exists for PcTools pytest files (no check runs them); `test_buildlock.py` also flakes under load (INFO-5), so wiring it is left to the owner.
- INFO-1, INFO-5, INFO-6: no change.

Negtests (`-Preset pytest`, scoped to `BuildLockReviewFixes` + `BuildLockBuildFx8`): N8, LOW-1 (timeout check removed) and LOW-2 (utime removed) all CAUGHT by a failing assertion.
