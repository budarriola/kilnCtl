# Adversarial review: ELF-archive lock fixes (commit `765371`..., 2026-09-15)

Scope: `tools/PcTools/src/kilnctrl/elf_archive.py` and
`tools/PcTools/tests/test_elf_archive.py` at the reviewed commit, which claims to
close every finding of `docs/audits/review_elf_lock_fixes_da6667f3_2026-09-15.md`.

Note on hashes: commits are named by 6-character prefixes throughout, so no
literal in this prose is mistaken for a full git hash. Reviewed commit `765371`;
its real parent `4696b9`; the prior review `da6667`; the parent the commit
message claims, `7a801a`.

Verification was done from a clean worktree of `origin/main` at the reviewed
commit, and a second clean worktree at its real parent for the negative runs.
No production source was modified; all three demonstrations below ran as
standalone scripts outside the repo, importing the worktrees' `src/` directly.

## Verdict summary

| Finding | Claim | Verdict |
|---|---|---|
| HIGH-1 stale-reclaim busy-spin ignores `timeout_s` | closed | **closed** |
| HIGH-2 lock mtime never refreshed while held | closed | **partially closed** |
| HIGH-3 no ownership token | closed | **partially closed** (release path only) |
| MED-1 `_archive()` proceeded unlocked on timeout | closed | **partially closed** |
| MED-2 only 3 of 12 tests load-bearing | closed | **not closed** |
| LOW-2 ELF copies outside the lock | closed | **closed** |

## HIGH-1 — closed

The staleness branch no longer `continue`s. It falls through to the deadline
check and the 50 ms sleep, so `timeout_s` now bounds every path including a
reclaim that keeps failing.

Confirmed by demonstration against the real parent `4696b9`: with `os.remove`
patched to raise `PermissionError` for the lock path (Windows' behaviour when a
live holder still has the handle open) and an artificially aged lock file, a
waiter with `timeout_s=0.5` had still not returned after 6 s, logging the
"treating as abandoned" warning repeatedly. The same scenario at the reviewed
commit returns within the deadline. The defect was real and is fixed.

Residual, LOW: after a *successful* reclaim the loop still checks the deadline
before retrying the create, so a caller with a short `timeout_s` can raise
`TimeoutError` on the very iteration in which it freed the lock. Harmless for
the 30 s default; visible for the 3 s lookup path.

## HIGH-2 — partially closed

A heartbeat now exists (`_LockHeartbeat`, `os.utime` every
`_LOCK_HEARTBEAT_SECONDS = 2.0`, against `_LOCK_STALE_SECONDS` lowered 10.0 to
6.0) and is correctly started after acquisition and stopped in the context
manager's `finally`, so exception exits are covered. SIGKILL and interpreter
exit are safe: the thread is a daemon, dies with the process, and the lock ages
out in 6 s. That much is sound.

Three problems remain.

**(a) The docstring is false.** `_LockHeartbeat.__doc__` says it "re-affirms its
owner token". `_run` only calls `os.utime`; `self._token` is stored and never
read. Demonstrated: after the lock file was replaced with a different token, the
heartbeat left the foreign token on disk untouched.

**(b) The heartbeat is path-keyed, not token-keyed — new defect, MEDIUM.**
Because it only touches a path, a holder whose lock was reclaimed keeps
refreshing *somebody else's* lock file forever. Demonstrated directly: holder A
started a heartbeat on its lock; the file was then removed and recreated with
B's token; A's heartbeat advanced B's file's mtime by 1.16 s over a 1.2 s
window, so a third waiter computed B's lock age as 0.05 s when its real age was
1.21 s. If B then dies, A's surviving thread masks B's death indefinitely and no
waiter can ever declare that lock stale — the exact liveness failure HIGH-2 was
raised to prevent, reintroduced on the heartbeat path. The fix is to re-read the
token in `_run` and stop when it is no longer ours (which is what the docstring
already claims happens).

**(c) The stale-reclaim "re-check the token" guard cannot see a heartbeat —
new defect, MEDIUM.** The reclaim path reads the token, prints, then re-reads and
removes only if the token is unchanged. Its inline comment states this covers the
case where "the holder's heartbeat ... touched the file since our stat above".
It does not: `os.utime` changes mtime, not content, so the token compares equal
and the waiter deletes a lock that was heartbeated microseconds ago. The guard
only detects a *replacement*, never a refresh. Correct fix is to re-`stat` the
mtime and abandon the removal if the age is now below the threshold.

**(d) Margin.** 2.0 s against 6.0 s is three intervals. Lowering 10.0 to 6.0
reduces absolute margin for legitimately long operations exactly when the
heartbeat thread is least likely to be scheduled — a GIL-bound or descheduled
process, or a slow filesystem. This tree is OneDrive-backed and
`tools/run_all_checks.ps1` runs 8-way parallel. Three missed `os.utime` calls is
not a large budget. Combined with (c), a false staleness declaration ends in an
actual deletion of a live holder's lock rather than a retry.

## HIGH-3 — partially closed

`_release_lock` compares the on-disk token to the holder's own before removing,
and the reclaim path compares before removing, so the release path is genuinely
fixed.

- Torn reads: the token is written with one `os.write` immediately after an
  exclusive create. A short read cannot compare equal, so the failure mode is a
  skipped removal, not a wrong removal. Fail-safe.
- Read failure: `_read_lock_token` returns `None` on `OSError`, which likewise
  compares unequal and skips the removal. Fail-safe — the lock leaks until it
  ages out.

Not covered: the heartbeat path, per HIGH-2 (b). A process can still keep a lock
it does not own alive. Ownership is enforced on deletion but not on refresh.

Also note `_release_lock_pre_high3` — a copy of the pre-fix unconditional-remove
logic kept in *production* source purely so a test can call it. LOW: dead,
hazardous-by-name code shipped in the module; it belongs in the test file.

## MED-1 — partially closed

`_archive()` now raises `TimeoutError` instead of proceeding unlocked, and the
copies are inside the lock. But:

**(a) `required=False` does not skip the write.** `migrate_legacy_archive`
passes `required=False`, and on timeout the lock loop sets `fd = None`, prints
"proceeding WITHOUT the lock", and **runs the full migration anyway**. The commit
message's justification ("skipping an opportunistic legacy-merge is harmless")
describes behaviour the code does not implement. Demonstrated: with holder A
inside the critical section, a `migrate_legacy_archive(..., lock_timeout_s=0.3)`
call logged the proceed-without-lock warning and then wrote `manifest.json`,
deleted the legacy `manifest.json`, and moved the legacy ELF — an unlocked
manifest read-modify-write concurrent with a lock holder, which is precisely the
clobber MED-1 named. Severity MEDIUM; the window is small (legacy archives are
a one-time migration) but the operation is destructive and unrecoverable.

**(b) The new `TimeoutError` is swallowed at the call site.**
`mcp_server_flash.py:403` wraps `archive_kiln_elf` in
`except Exception as exc:  # noqa: BLE001 - archiving is a diagnostic
convenience, never fail the flash over it`. So MED-1's fix converts "silently
writes unlocked" into "silently archives nothing" — better, but the operator is
not told, and the ELF needed to symbolize a future crash is simply absent.

M3's "lookups never block or raise" property **is** preserved: both lookup sites
use `lock_timeout_s=3.0`, `required=False`, and their own `try/except Exception`.

## MED-2 — not closed

The commit message states the new tests were validated against parent `7a801a`
and that 6 of 8 fail there. Two problems.

**Wrong baseline.** `7a801a` is not the reviewed commit's parent — it is the
prior review's parent, an audit-doc commit. The real parent is `4696b9`. On the
two files in scope, `git diff --stat da6667 4696b9` is empty (so `4696b9` carries
the `da6667` code, which is what the review was written against) while
`git diff --stat 7a801a 4696b9` shows 538 + 288 changed lines. The implementer
measured "does my test fail?" against code 826 lines older than what they were
fixing.

**Per-test result against the real parent `4696b9`** (new test file overlaid on
parent `src/`; whole file: 5 failed, 63 passed, 2 teardown errors):

| `ArchiveLockTest` test | vs real parent | why |
|---|---|---|
| `test_contention_serializes_two_holders` | PASSES | parent already serialized |
| `test_owned_lock_is_removed_on_clean_exit` | PASSES | parent already removed on exit |
| `test_stale_lock_is_reclaimed_without_waiting_the_full_timeout` | PASSES | parent already reclaimed stale locks |
| `test_deadline_is_honored_even_when_stale_reclaim_keeps_failing` | FAILS | `TypeError: _archive_lock() got an unexpected keyword argument 'required'` — signature artifact |
| `test_heartbeat_prevents_false_staleness_during_a_long_critical_section` | FAILS | `AttributeError: ... does not have the attribute '_LOCK_HEARTBEAT_SECONDS'` — artifact |
| `test_negative_reproduces_the_lock_steal_without_the_owner_check` | FAILS (+teardown `PermissionError` WinError 32) | `AttributeError: module 'kilnctrl.elf_archive' has no attribute '_read_lock_token'` — artifact |
| `test_reclaim_never_deletes_a_newer_holders_lock` | FAILS (+teardown `PermissionError` WinError 32) | same `AttributeError` — artifact |
| `test_timeout_waiter_does_not_delete_a_live_holders_lock` | FAILS | same `AttributeError` — artifact |

Every failure is a missing symbol or a changed signature. **Zero of the eight
tests fails against the real parent for a behavioural reason.** HIGH-1's
busy-spin is genuinely reproducible at the parent (demonstrated above) — but the
test written for it cannot show that, because it aborts on the `required` kwarg
before reaching the timing assertion. The three tests that do pass at the parent
pass because the parent behaved correctly in those respects, not because they
are strong.

All 60 non-`ArchiveLockTest` tests pass at the parent, including both
`KilnFwLegacyMigrationLookupTest` tests and both
`LegacyMergeMissingFileAndRenumberingTest` tests — the de-vacuum-ing of the
migration-lookup test did not make it load-bearing either.

**What would catch a reintroduction, against the current API:**
`test_deadline_is_honored...` catches HIGH-1; `test_reclaim_never_deletes_a_newer_holders_lock`
catches HIGH-3 on the release path; `test_heartbeat_prevents_false_staleness...`
catches deletion of the heartbeat, but weakly — it asserts only
`assertIsNotNone(still_holder_token)`, never that the token is still *ours*, so
it passes if a different process's lock is sitting there.

**Uncovered by any test:** the heartbeat refreshing a foreign lock (HIGH-2 b);
the utime-versus-token gap in the reclaim re-check (HIGH-2 c); the unlocked
write performed by `required=False` (MED-1 a); the swallowed `TimeoutError` at
the flash call site (MED-1 b).

## LOW-2 — closed

Both `shutil.copyfile` calls (archived ELF and `latest.elf`) are inside
`with _archive_lock(archive_dir)`, alongside the migration, adoption, manifest
read/write and prune. `_sha256_key(elf_path)` remains outside, which is correct —
it only reads the source ELF.

## New defects introduced by this commit

1. **MEDIUM** — heartbeat is path-keyed, so a stale thread refreshes another
   process's lock indefinitely and masks that holder's death. Demonstrated.
2. **MEDIUM** — reclaim's token re-check cannot detect a heartbeat (`os.utime`
   leaves content identical), so a live, freshly-refreshed lock can be deleted.
   The inline comment asserts the opposite.
3. **MEDIUM** — `required=False` performs the full unlocked migration write
   rather than skipping it. Demonstrated writing the manifest and deleting
   legacy files while another holder was inside the lock.
4. **LOW** — `_LockHeartbeat`'s docstring claims an owner-token re-affirmation
   that does not exist; `self._token` is dead state.
5. **LOW** — `_release_lock_pre_high3` ships the pre-fix unconditional-remove
   logic in production source for a test's benefit.
6. **LOW** — `_LOCK_STALE_SECONDS` lowered 10.0 to 6.0 shrinks the margin for
   legitimately long operations on this OneDrive-backed, 8-way-parallel tree,
   and defect 2 makes an erroneous staleness call destructive rather than
   merely a wasted retry.

## Suite state

At the reviewed commit, in a clean worktree: `test_elf_archive.py` alone,
68 passed in 6.52 s. Full `tools/PcTools/tests/` with `-n auto`: 22 failed,
2343 passed, 23 skipped in 446 s. All 22 failures are pre-existing and unrelated
to `elf_archive` — `test_coupled_ident.py` (5), `test_pid_ab_compare.py` (12),
`test_load_estimator.py` (2) all need untracked `logs/coupling/*.jsonl`
fixtures; `test_logic_capture_paths.py` (1) and `test_mcp_server_saleae_paths.py`
(2) are out-dir-already-exists assertions. This matches LOW-1 of the prior
review.
