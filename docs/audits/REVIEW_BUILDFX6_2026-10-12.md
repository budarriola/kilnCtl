# Review: BUILDFX6 (7eee14292, 7d39654db), 2026-10-12

Adversarial review of buildfx6's fixes for `REVIEW_BUILDFX5_2026-10-12.md`
LOW-1, LOW-2 and INFO-5, all in `tools/PcTools/src/mcpkit/buildlock.py`:

- `7eee14292`: the `BuildLockTimeout` message names the lock path and flags
  a holder that is this same process. `_release` retries the aside read and
  logs to stderr when the rename-aside gives up. The reclaim sidecar carries
  a token. `_release_sidecar` removes the sidecar only when the token matches
  and restores a foreign one. A confirmed-alive holder returns before the
  sidecar is taken. Tests are in `BuildLockBuildFx6`.
- `7d39654db`: docs (`docs/MCP_SERVERS.md`, the BUILDFX5 status lines).

Reviewed in `C:\wt\rvbuildfx6_*` at origin/dev `0e260bd02`. Line numbers
below are for that tree.

**Verdict: 0 HIGH, 0 MED, 2 LOW, 6 INFO.** The fixes do what BUILDFX5 asked.
Each one narrows its window without closing it, and the narrower residuals
are not documented. Nothing here can block a build forever that could not
before. The one permanent leak (LOW-2) is the case BUILDFX5 LOW-1 named.
The retry now makes it rarer, but it is still silent.

## Answers to the review questions

| Question | Answer |
|---|---|
| Can two reclaimers both remove the lock? | Yes, but only in a narrower window than before. The token check (`:297`) is check-then-act. A reclaimer that passed it and is still inside `_remove_retry(lock_path)` keeps deleting whatever file is at the path. That retry loop can last seconds while waiters read the lock. See LOW-1, repro G1. Getting a second reclaimer in still needs the BUILDFX5 LOW-2 sidecar eviction (a three-way race, or a stall of more than 30 s). |
| Can restoring a foreign sidecar resurrect a stale one? | It cannot make a stale one live. On NTFS a rename keeps the mtime, so a restored stale sidecar is still broken by the next `_break_stale_sidecar`; `test_release_sidecar_restores_a_foreign_stale_sidecar_still_stale` checks this. The restore can put back a sidecar whose owner already finished. That orphan blocks reclaims for at most 30 s (INFO-2, repro G5). |
| Can the retry loop mistake a new holder's aside for its own? | No. The compare is against `my_text`, which contains a per-acquire uuid. An empty or unreadable aside is always restored, never deleted (`test_release_keeps_a_new_holders_lock_that_never_became_readable`, `test_release_restores_new_holder_whose_write_lands_during_retry`). The retry does sleep while `lock_path` is vacant, which widens the restore vacancy (INFO-1, repro G3). |
| Can own-process detection misfire across a PID reuse? | Not in practice. It needs the same PID, a token, and a recorded start within 2 s of this process's start (`:64-72`). An earlier process with a reused PID would have to start, take the lock, leak it and die within those 2 s. The only effect would be one wrong sentence in an error message. `test_own_process_detection_rejects_pid_reuse_and_tokenless` checks this. A lock legitimately held by another thread of the same server is reported as "THIS server process" (INFO-4). |
| Can any path now leak the lock permanently, or block a build forever? | Yes, one path, and it is the same one as before: `_release` restores its own lock when the aside read still fails after the four retries. A confirmed-alive holder is never reclaimed, so that leak lasts until the server restarts, and nothing is logged (LOW-2, repro G2). Nothing new blocks forever. Every new sidecar failure path leaves a sidecar that the 30 s break removes. |

## LOW

### LOW-1 A reclaimer past the token check still deletes whatever lock its retry finds

`buildlock.py:297-299` and `_remove_retry` `:193-205`.

The new check `if _read_holder(sidecar) != token: return` runs once, just
before `_remove_retry(lock_path)`. `_remove_retry` deletes by path, not by
identity. On a sharing violation it retries for up to 400 attempts. BUILDFX5
measured 400 attempts at about 7 s on this machine. A sharing violation is
the normal case under contention, because every waiter's `_read_holder` opens
the lock without `FILE_SHARE_DELETE`. During that loop:

- if the reclaimer's sidecar is evicted, the BUILDFX5 LOW-2 vacancy, which
  is still there, lets a second reclaimer in. Its re-read still sees the
  judged dead lock, because the first reclaimer's delete is the one being
  refused, so it passes its own token check and also deletes;
- or the judged lock belonged to an alive holder that is not confirmed
  (legacy content, or start time unknown) and it is being age-reclaimed. That
  holder's own `_release` removes it.

Either way, once the dead lock is gone, a new holder H can `O_EXCL` the path.
The next retry then deletes H's live lock, and a third caller acquires beside
H. Repro G1 (deterministic, one injected `PermissionError` on the lock
delete) leaves H's lock deleted.

The fix's status line says "the lock is removed only while the sidecar is
still ours". That holds only up to the token check. The window shrank from
the whole reclaim body to the delete retry, but the retry is the slow part.

Fix: delete the judged lock by rename-aside and verify, as `_release` already
does. Rename `lock_path` to `lock_path.rec.<uuid>` and compare the grabbed
content with `judged`. Remove the file only if they are equal; otherwise
rename it back. A second reclaimer then gets `FileNotFoundError`, or grabs
H's lock, sees a mismatch and restores it. The residual becomes the
microsecond restore vacancy that `_release` already documents.

### LOW-2 `_release` still leaks its own lock silently when the aside read keeps failing

`buildlock.py:398-410`.

BUILDFX5 LOW-1 asked for the read to be retried and for a give-up to be
logged. The retry is there (four retries, 80 ms in all). After the last
retry, `held` is None, so the lock is restored to `lock_path`. That is the
safe choice, because the file may be a new holder's half-written lock. But
for a confirmed-alive server it is a permanent leak. Only the rename give-up
path prints `LEAKED LOCK`; this path prints nothing. An AV or indexer handle
that lasts longer than 80 ms is enough to reach it. Repro G2: `_read_holder`
always None, then the lock is still there and stderr is empty.

Fix: print the same `LEAKED LOCK` line, naming the path, before the restore
when `held is None` after the retries (`"could not read <aside>; restored
it; if it was ours it is leaked"`). Better still, restore first and then
retry the whole rename-and-read, so the decision is never made blind.

## INFO

### INFO-1 The read retry sleeps with the lock path vacant

`buildlock.py:398-403`. `_release` renames the lock aside and then sleeps up
to 4 x 20 ms before restoring a file it cannot read. The case that triggers
the retry, an empty file, is a new holder's lock caught between `O_EXCL` and
`write`. That case is exactly the foreign one, and in it the vacancy has
grown from microseconds to as much as 80 ms. A waiter that `O_EXCL`s the
vacant path wins. The restore then fails, and the new holder's lock is
deleted (repro G3). `_release` only reaches a foreign lock after its own lock
was reclaimed. For a confirmed-alive holder (any Windows process that can
read its own start time) that never happens, so this is INFO. The
"restore first, then retry" fix in LOW-2 also removes this.

### INFO-2 `_release_sidecar`'s foreign restore is a new vacancy, and the residual is not documented

`buildlock.py:244-264`. Restoring a foreign sidecar is rename-aside, read,
then rename back. A third reclaimer can `O_EXCL` the vacant sidecar path
during that window. The restore then fails and the foreign (live) sidecar is
deleted, so its owner and the newcomer are both inside (repro G4). This is
the same shape as BUILDFX5 LOW-2. It is reachable only once the releaser's
own sidecar was already evicted. The token check (`:297`) stops the evicted
owner from deleting the lock unless it is already past that check
(LOW-1). It can also put back the sidecar of a reclaimer that finished
during the vacancy (its own release saw `FileNotFoundError`). That orphan
blocks every reclaim for up to 30 s (repro G5). BUILDFX5 LOW-2 asked for
this residual to be documented "next to `_release`'s note" if it was
accepted. Neither `_release_sidecar`'s docstring nor the module docstring
mentions it.

### INFO-3 Sidecar failure paths are silent and asymmetric with `_release`

`buildlock.py:248-257` and `:283-288`. `_release_sidecar` has no read retry,
and it does not log when the rename gives up. `_try_reclaim` ignores a failed
token write. Each of these leaves the reclaimer's own sidecar in place, so
reclaims stall for up to 30 s until `_break_stale_sidecar` removes it. That
is bounded and not a leak. A one-line stderr note would make a 30 s stall
explainable.

### INFO-4 Same-process message for a live in-process holder

`buildlock.py:87-94`. `build_kilnfw_start`, `bench_test_start` and the other
`*_start` tools run builds on server threads. A timeout against a build that
another thread is running reports the holder as "THIS server process" and
suggests deleting the lock file. The message does say "if no build is running
here", but deleting the file during a live build would allow two concurrent
builds. BUILDFX5 LOW-1's first suggestion, an in-process set of held keys,
would tell a live holder from a leaked one. A cheaper fix is to put "check
`build_job_status` first" in the message.

### INFO-5 Module docstring is stale

`buildlock.py:32-37`. The docstring still describes the sidecar as an O_EXCL
file only. It does not mention the token, the token-matched release, or the
restore of a foreign sidecar.

### INFO-6 INFO-5 early return is correct

`buildlock.py:273-274`. `_try_reclaim` returns before taking the sidecar for
a confirmed-alive `judged`. The re-check against the re-read `current`
(`:293`) is still there. A judged text that is confirmed alive cannot also
have been judged dead, because a dead judgement needs a dead PID or a start
mismatch. So the early return only skips the age path, which the inner check
would have refused anyway. M09 is caught by the existing
`test_confirmed_alive_holder_never_touches_sidecar`.

## Tests added

In `tools/PcTools/tests/test_buildlock.py`, class `BuildLockReviewBuildFx6`:

- `test_concurrent_reclaimers_admit_exactly_one_new_holder`: 8 threads, 10
  rounds, real filesystem. A random 0-10 ms delay is injected into the dead
  check. Every thread reclaims the same dead lock, then races to `O_EXCL` a
  new one. Nobody releases, so a second winner means a live lock was deleted.
- `test_release_sidecar_restores_a_foreign_stale_sidecar_still_stale`.
- `test_release_keeps_a_new_holders_lock_that_never_became_readable`.
- `test_release_restores_new_holder_whose_write_lands_during_retry`.
- `test_own_process_detection_rejects_pid_reuse_and_tokenless`.

Gap repros G1-G5 are scratch scripts (`repro_gaps.py`), not committed. They
assert current behavior that is wrong, so they would fail.

## Negative tests

`tools\negtest.ps1 -Preset pytest -PresetArg tests/test_buildlock.py`, with
the mutations in a private scratch JSON. Each CAUGHT was confirmed in the log
to be an `AssertionError` from a named test.

| ID | Mutation (`buildlock.py`) | Verdict | Caught by (AssertionError) |
|---|---|---|---|
| M01 | `_release_sidecar` deletes a foreign sidecar instead of restoring it | CAUGHT | `test_reclaim_does_not_remove_lock_after_its_sidecar_was_replaced`, new `test_release_sidecar_restores_a_foreign_stale_sidecar_still_stale` |
| M02 | `_release_sidecar` ignores the token (`if True:`) | CAUGHT | same two |
| M03 | `_holder_is_this_process` ignores the start time (PID reuse) | CAUGHT | new `test_own_process_detection_rejects_pid_reuse_and_tokenless` |
| M04 | `_holder_is_this_process` accepts tokenless (legacy) content | CAUGHT | new `test_own_process_detection_rejects_pid_reuse_and_tokenless` |
| M05 | `_release` treats an unreadable aside as its own (`or held is None`) | CAUGHT | new `test_release_keeps_a_new_holders_lock_that_never_became_readable` |
| M06 | `_release` read retry removed (`range(0)`) | CAUGHT | `test_release_retries_transient_aside_read_failure`, new `test_release_restores_new_holder_whose_write_lands_during_retry` |
| M07 | No reclaim exclusivity (sidecar without `O_EXCL` and no token check) | CAUGHT | new `test_concurrent_reclaimers_admit_exactly_one_new_holder` (`7 != 1` winners), `test_eight_processes_dead_pid_lock_never_overlap`, two others |
| M08 | Token check before the lock delete removed | CAUGHT | `test_reclaim_does_not_remove_lock_after_its_sidecar_was_replaced` |
| M09 | INFO-5 early return for a confirmed-alive holder removed | CAUGHT | `test_confirmed_alive_holder_never_touches_sidecar` |
| M10 | Foreign sidecar restored by re-creating it, which refreshes the mtime | CAUGHT | new `test_release_sidecar_restores_a_foreign_stale_sidecar_still_stale` |

M01 to M09 ran in one negtest run (baseline PASS, real tree unchanged). The
first M10 mutant failed to import (a `'
'` escaping error), so it did not
count. It was fixed to use `chr(10)` and re-run on its own (baseline PASS,
CAUGHT by assertion, real tree unchanged).

LOW-1, LOW-2, INFO-1 and INFO-2 are behavior gaps, not missing tests, so
mutation testing does not apply to them. They are shown by repros G1 to G5:
G1 deletes a live lock, G2 leaks silently, G3 deletes a new holder's lock
across the retry sleep, G4 evicts a live sidecar, and G5 resurrects an
orphan sidecar.

## Fix status

Fixed in buildfx7 (tools/PcTools/src/mcpkit/buildlock.py, tests in `BuildLockBuildFx7`):

- LOW-1 fixed: `_try_reclaim` now deletes via `_remove_judged` (rename-aside, compare with the judged content, restore on mismatch); a retry can no longer delete a new holder's lock. Test from repro G1.
- LOW-2 fixed: `_release` logs `LEAKED LOCK?` to stderr naming the path when the aside stays unreadable. Test from repro G2.
- INFO-1, INFO-2, INFO-3 documented as accepted residual windows in the module docstring; INFO-5 docstring updated; INFO-4 message now says to check `build_job_status` first. INFO-6 needs nothing.
