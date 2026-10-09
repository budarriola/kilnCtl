# Adversarial review, round 3: ELF-archive lock fixes (commit `f4da67`, 2026-09-15)

Scope: `tools/PcTools/src/kilnctrl/elf_archive.py` and
`tools/PcTools/tests/test_elf_archive.py` at the reviewed commit, which claims to
close the three MEDIUM defects and three LOWs raised by
`docs/audits/review_elf_lock_fixes_7653717f_2026-09-15.md`.

Note on hashes: commits are named by 6-character prefixes throughout, so no
literal in this prose can be mistaken for a full git hash. Reviewed commit
`f4da67`; its parent `8813be`; the code baseline the tests are measured against
`4696b9`; the two prior reviews `f64315` and `6e58b2`.

Verification was done from a clean, uniquely-named worktree of `origin/main`
(confirmed: `origin/main` *is* `f4da67`; the main tree's local `main` is
diverged and was not used, moved, or touched). Parent-baseline runs used a
scratch package copy outside the repo — a real `kilnctrl/` package with only
`elf_archive.py` swapped for the `4696b9` version, so relative imports resolve.
No production source was modified; the worktree's `git diff` was confirmed empty
after every experiment.

## Verdict summary

| Claim | Verdict |
|---|---|
| HIGH-2 (b) heartbeat re-reads the token, stops on mismatch | **closed**, with a named bounded residual |
| HIGH-2 (c) reclaim re-stats mtime instead of comparing tokens | **closed**, with a named bounded residual |
| MED-1 (a) `_LockNotAcquired` raised before `yield` | **closed** |
| Residual LOW on HIGH-1 (`continue` after a successful reclaim) | **closed** |
| LOW: heartbeat docstring corrected | **closed** |
| LOW: `_release_lock_pre_high3` moved into the test file | **closed** |
| LOW: margin widened 2.0/6.0 to 1.0/10.0 | **closed** |
| MED-1 (b) `mcp_server_flash.py` blanket `except` left unchanged | **acceptable resolution**, not a deferred defect |
| Per-test parent table in the commit message | **overstated** — see below |

Four new LOWs, no new MEDIUM or HIGH. Details at the end.

## HIGH-2 (b) — closed, residual named

`_run` now reads the on-disk token and returns on mismatch before calling
`os.utime`. The defect the prior review demonstrated (a stale thread refreshing
a *foreign* lock indefinitely, masking the new holder's death forever) is gone:
the thread exits permanently on the first mismatched tick.

The read-then-`utime` sequence is not atomic, and a reclaim can land between
them. The consequence is bounded and benign, which is the important difference
from the defect being fixed:

- the stray `os.utime` sets the new holder's mtime to *now*, which is exactly
  what that holder's own heartbeat does anyway;
- the very next tick re-reads, mismatches, and the thread exits.

So the worst case is a single spurious refresh, extending a *dead* new holder's
apparent liveness by at most one heartbeat interval (now 1.0 s) against a 10.0 s
staleness threshold. The prior defect was unbounded — indefinite masking. This
is a genuine close, not merely a narrowing, because the failure mode changed
class rather than shrinking.

Exit paths are sound. `stop()` is called from the context manager's `finally`,
so exception exits are covered; the thread is a daemon, so interpreter shutdown
and SIGKILL are covered (the lock then ages out). One note: `stop()` joins with
`timeout=1.0`, now equal to the heartbeat interval. `Event.wait` returns
immediately on `set()`, so a join timeout requires the thread to be blocked
inside `_read_lock_token`/`os.utime` for over a second — plausible on this
OneDrive-backed tree. If that happens, `_release_lock` proceeds anyway and the
surviving thread can land one stray `utime` on a successor's lock: the same
bounded single-refresh window as above, not a new class of problem.

## HIGH-2 (c) — closed, residual named

The reclaim path now computes `fresh_age` from a second `os.path.getmtime`
immediately before `os.remove`, and abandons the removal if the lock is no
longer stale. This is the correct fix and strictly stronger than the token
compare it replaces: it detects a refresh (which `os.utime` makes invisible to a
content compare) *and* still handles a replacement, because a new holder's
freshly-created file has a fresh mtime.

The stat-then-remove window remains open in principle. For it to bite, a holder
must have missed ten consecutive heartbeats (age > 10.0 s with a 1.0 s interval)
and then land a `utime` inside the microseconds between the two stats. That is
acceptable: the guard is a best-effort narrowing on top of a threshold already
carrying ten intervals of margin, and the code says so.

The widened margin is the right direction for this tree. Note it is not a
widening relative to the pre-heartbeat baseline — `4696b9` already used
`_LOCK_STALE_SECONDS = 10.0`; round two lowered it to 6.0 and this round
restores 10.0 while adding a 1.0 s heartbeat under it.

## MED-1 (a) — closed

`_LockNotAcquired` is raised inside the acquire loop, before `yield`. The
contextmanager semantics are correct and worth stating explicitly, since this is
the kind of thing that looks right and is not:

- `contextlib._GeneratorContextManager.__enter__` calls `next(self.gen)`; an
  exception raised before the `yield` propagates out of `__enter__`;
- because `__enter__` never returned, the `with` statement never binds and
  `__exit__` is never called, so the generator's `finally` never runs;
- that `finally` is exactly the code that would have released a lock — and at
  that point `fd` is `None` and nothing was acquired, so there is nothing to
  release. No caller cleanup runs expecting a lock that was never taken.

`migrate_legacy_archive` is the only `required=False` caller (confirmed by grep
across `tools/`) and catches it. The M3 "lookups never block or raise" property
still holds: both lookup sites pass `lock_timeout_s=3.0` and additionally wrap
the call in their own `except Exception`, so the bound is unchanged and nothing
escapes.

## MED-1 (b) / `mcp_server_flash.py` — acceptable resolution

I assessed this independently rather than taking the reasoning on trust, because
the prior review's stated grounds for raising it ("the operator is not told")
would make silent provenance loss a real cost: an unarchived ELF means a later
panic cannot be symbolized, and the failure would surface only when someone
needs it.

The premise no longer holds. `_archive_flashed_elf`'s `except Exception` returns
`"\nWARNING: elf archiving FAILED (flash itself succeeded): {exc}"`, and both
success paths in `flash_firmware` concatenate that return value into the string
the tool actually returns (`mcp_server_flash.py` lines 730-731 and 741-742). The
warning is therefore in the tool result the operator reads, not only in a log
line — the distinction the prior review's finding turned on. The exception text
itself is interpolated, so a `TimeoutError` or `_LockNotAcquired` arrives
named and legible.

"Never fail the flash over archiving" is the right contract, and with the
failure surfaced in the returned message the degradation is no longer silent.
This is a resolution, not a deferral. The residual is stylistic: a blanket
`except Exception` will also absorb a genuine programming error in the archive
path and report it as an archiving failure — acceptable for a diagnostic
convenience, and explicitly `# noqa`-justified in place.

## Per-test parent table — my own measurement

The commit message's table adapts each test past the missing-API artifacts and
then reports why it would fail. That is a legitimate thing to do, but it is not
what "overlay the test file on the parent" produces, and the adapted results are
not independently checkable from the table alone. Below is the unadapted
overlay — `4696b9`'s `elf_archive.py` with the reviewed commit's test file, run
per-test under pytest (the repo's real runner) — followed by my own probes of
each behavioural claim, run against the parent directly.

| `ArchiveLockTest` test | unadapted vs parent | classification |
|---|---|---|
| `test_contention_serializes_two_holders` | PASSES | not load-bearing; parent already serialized |
| `test_stale_lock_is_reclaimed_without_waiting_the_full_timeout` | PASSES | not load-bearing; parent already reclaimed |
| `test_owned_lock_is_removed_on_clean_exit` | PASSES | not load-bearing; parent already removed on exit |
| `test_required_false_skips_the_migration_entirely_on_timeout` | **FAILS: `AssertionError: 1 != 0`** | **genuinely load-bearing, no adaptation needed** |
| `test_timeout_waiter_does_not_delete_a_live_holders_lock` | errors: no `_read_lock_token` | API artifact; behaviour confirmed separately (probe 3) |
| `test_negative_reproduces_the_lock_steal_without_the_owner_check` | errors: no `_read_lock_token` | API artifact; N/A — exercises a test-local helper, not `_archive_lock` |
| `test_reclaim_never_deletes_a_newer_holders_lock` | errors: no `_read_lock_token` | API artifact; parent has no token at all |
| `test_deadline_is_honored_even_when_stale_reclaim_keeps_failing` | errors: no `_LockNotAcquired` | API artifact; behaviour confirmed separately (probe 1) |
| `test_heartbeat_prevents_false_staleness_during_a_long_critical_section` | errors: no `_LOCK_HEARTBEAT_SECONDS` | API artifact; behaviour confirmed separately (probe 2) |
| `test_heartbeat_stops_refreshing_a_lock_it_no_longer_owns` | errors: no `_LOCK_HEARTBEAT_SECONDS` | API artifact; parent has no heartbeat to test |
| `test_reclaim_does_not_delete_a_lock_refreshed_by_a_heartbeat` | errors: no `_LockNotAcquired` | API artifact |

Note the parent's `_archive_lock` never writes a token at all (it opens the file
and breaks without `os.write`), so every token-based test is testing a facility
that does not exist there — those are new-feature tests, not regression tests
against the parent, and no adaptation can change that.

Three probes against the parent, each confirming a behavioural claim the table
makes:

1. **Deadline (HIGH-1).** With `os.remove` forced to fail for the lock path and
   an artificially aged lock, a `timeout_s=0.5` acquire at the parent had still
   not returned after 8.05 s. The hang is real; `test_deadline_is_honored...`
   would catch its reintroduction.
2. **Long critical section (HIGH-2).** With a live holder inside its section and
   the staleness threshold shrunk to 1.0 s, the parent's reclaim path attempted
   **18,726 removals of the live holder's lock**, every one refused by Windows
   (`WinError 32`, the holder's own open handle). The holder survived; the code
   never decided not to delete it.
3. **Unlocked body (MED-1 a).** A `timeout_s=0.3` waiter at the parent entered
   the critical section while the holder was still inside it — mutual exclusion
   violated, confirming the test's premise directly.

**On probe 2 and whether that test is meaningful or accidental.** The commit
message is candid that only Windows' file lock prevented the delete, and asks
implicitly whether that makes the test accidental. It does not. 18,726 attempted
deletions of a live lock is the code expressing the wrong decision 18,726 times;
the OS merely declined to carry it out. The protection is platform-specific and
holder-state-specific — it depends on the holder still having the fd open, which
is precisely what is *not* true for the reclaim case the mechanism exists to
serve, and would not hold on POSIX at all. The test asserts the code reaches the
right decision rather than that the file survives, so it is meaningful. What is
accidental is the parent's *survival*, not the test.

**Net, honestly stated:** of 11 tests in the class, 3 pass at the parent and are
not load-bearing; 1 is N/A (a test-local helper); 1 fails at the parent with no
adaptation whatsoever; and 6 cannot run at the parent because they test APIs
that do not exist there, 3 of which cover behaviour I independently confirmed is
broken at the parent. The commit message's "4 of 7 applicable" is defensible
arithmetic but counts adapted variants of tests that, as committed, cannot run
against the baseline. The honest headline is **1 of 11 demonstrably load-bearing
as written**, with 3 more load-bearing in substance via adaptation.

**What catches what on reintroduction:** HIGH-1 busy-spin →
`test_deadline_is_honored...`; MED-1 (a) unlocked body →
`test_required_false_skips_the_migration_entirely_on_timeout` (strongest test in
the class) and `test_timeout_waiter_does_not_delete_a_live_holders_lock`;
HIGH-2 (b) → `test_heartbeat_stops_refreshing_a_lock_it_no_longer_owns`;
HIGH-2 (c) → `test_reclaim_does_not_delete_a_lock_refreshed_by_a_heartbeat`;
HIGH-3 release path → `test_reclaim_never_deletes_a_newer_holders_lock`. The
prior review's specific critique — `assertIsNotNone` where token equality was
meant — is addressed; that assertion now compares the exact token.

**Covered by nothing:** the two residual races named above; the heartbeat thread
dying silently mid-critical-section (nothing observes or reports it, and the
holder would then be reclaimable while alive); and the four new LOWs below.

## New defects

1. **LOW — `_release_lock`'s signature now lies, and the lie is a `TypeError`.**
   The parameter is still typed `fd: Optional[int]`, but the `if fd is None:
   return` guard was removed along with the branch that produced `None`.
   Confirmed by direct call: `_release_lock(path, None, tok)` raises
   `TypeError: 'NoneType' object cannot be interpreted as an integer` from
   `os.close`, which the surrounding `except OSError` does not catch. Unreachable
   from production today — `fd` is always a valid descriptor by the time the
   `finally` runs — but the annotation invites the call that breaks it, and the
   docstring says the function is split out "so it can be exercised directly by
   tests". Narrow the annotation to `int` or restore the guard.
2. **LOW — only `FileExistsError` is caught around the exclusive create.** Any
   other `OSError` from `os.open` (a `PermissionError` from OneDrive sync
   touching the lock file, or a delete-pending name on Windows after a
   successful reclaim) escapes `_archive_lock` entirely — not as `TimeoutError`
   or `_LockNotAcquired`, but as a raw `OSError`, bypassing the module's whole
   acquire-failure contract. Both lookup sites absorb it in their own
   `except Exception`, and `_archive()`'s escapes into
   `_archive_flashed_elf`'s blanket handler, so today it degrades rather than
   crashes — but a `required=False` caller that catches only `_LockNotAcquired`,
   exactly as the new exception's docstring instructs, would not be protected.
3. **LOW — `migrate_legacy_archive` returns 0 for two different outcomes.**
   "Nothing to migrate" and "lock unavailable, migration skipped" are
   indistinguishable to a caller. Both production call sites discard the return
   value, so this is latent, and the skip is logged — but the new test asserts
   `result == 0` as the success criterion for the skip path, which is the same
   value a successful no-op returns, so that assertion is weaker than it reads.
   A sentinel (`-1`) or a returned tuple would make the skip observable.
4. **LOW — the widened threshold lengthens recovery from a genuinely dead
   lock.** With `_LOCK_STALE_SECONDS = 10.0` and lookups passing
   `lock_timeout_s=3.0`, a lookup meeting a lock left by a crashed holder less
   than 10 s ago can never reclaim it within its own timeout; it takes up to
   four successive lookups before one finds the lock old enough. Self-healing
   and correct in direction (the alternative is stealing live locks), but it
   should be understood as the deliberate cost of the widened margin rather than
   a surprise.

## Suite state

`tools/PcTools/tests/test_elf_archive.py` at the reviewed commit, clean
worktree, under pytest: **71 passed in 8.90 s**. The commit message's 71/71 claim
reproduces exactly.

One methodological note for future rounds, since it cost time here and would
mislead anyone spot-checking this file: the same 71 tests run under
`python -m unittest` report 4 failures in `CanonicalArchiveWriteGuardTest`. That
is an artifact of the runner, not a defect — `_guard_against_test_write` keys on
`PYTEST_CURRENT_TEST`, which only pytest sets, so the guard cannot fire and the
four tests asserting `assertRaises(RuntimeError)` fail. The same four fail
identically at the parent. This suite must be run under pytest.

Roughly 22 pre-existing failures elsewhere in `tools/PcTools/tests/` (missing
`logs/coupling/*.jsonl` fixtures and out-dir assertions) are unrelated to this
module and were not re-measured.

## Is this area safe to stop iterating on?

Yes, with the four LOWs above left as cleanup rather than blockers.

Rounds one and two each found that the previous round's fixes were incomplete,
and round two additionally found a load-bearing claim that was not load-bearing.
This round is different in kind: every claimed fix is genuinely in the code and
does what it says, the two residual races are bounded and correctly
characterised rather than papered over, the previously-rejected judgement call
on `mcp_server_flash.py` holds up on independent inspection, and the
verification claim reproduces. No new MEDIUM or HIGH defect was found.

The remaining honesty gap is in the commit message's per-test accounting, not in
the code: "4 of 7 applicable" describes adapted tests, while the tests as
committed yield 1 of 11 measurable against the baseline. That is a documentation
correction, not another round of fixes. The lock itself is now in a state where
further iteration would cost more than it returns.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
