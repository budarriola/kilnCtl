# Review: BUILDFX4 (c08a38d3a + 448c511f2), 2026-10-12

Adversarial review of the BUILDFX4 fixes for `REVIEW_BUILDFX3_2026-10-12.md`:

- `c08a38d3a`: `mcpkit/buildlock.py` O_EXCL `.reclaim` sidecar, token in the
  lock, token-checked release, holder process start time; `mcpkit/jobtree.py`
  KILL_ON_JOB_CLOSE Job object for every `workbench._run` child; `PidInfoBranches`
  tests; `fixture_flash` 0-byte refusal.
- `448c511f2`: same-content reclaim test; fix-status table in the BUILDFX3 doc.

Reviewed in `C:\wt\rvbuildfx4_*` at origin/dev `648e97e6d`. None of the
reviewed files changed after `448c511f2`.

**Verdict:**

- BUILDFX3 MED-1 (two waiters reclaiming a dead-PID lock) is fixed for the
  path it named. The sidecar serializes reclaimers, and the content and
  liveness re-checks inside it are both needed and both tested.
- BUILDFX3 MED-2 is fixed. The Job object works (killing a holder kills its
  build child), the struct layout is right, and no handles leak.
- LOW-1, LOW-2 and LOW-3 are fixed and their tests bite.
- **One MED, three LOW.** The MED is not new code. It is the age-based
  `force` path, which BUILDFX4 kept, and which now contradicts the build
  ceiling: a live build that holds the lock for more than 35 minutes is
  reclaimed, and two builds then share one directory. All four were
  reproduced on this machine.

## MED

### MED-1 The age path force-reclaims a live holder; a build may legitimately hold the lock for 119 minutes

`tools/PcTools/src/mcpkit/buildlock.py:272-273`, `:207`, default `stale_after`
at `:227`, called with that default from `workbench.py:358`.

When the lock file is older than `stale_after` (2100 s), the waiter calls
`_try_reclaim(..., force=True)`. With `force=True` the liveness re-check at
`:207` is skipped. The lock is removed even though the holder's PID is alive
and its recorded process start time matches.

2100 s is shorter than how long a holder can legitimately hold the lock:

- `build_kilnfw` now runs with `timeout=_build_ceiling_seconds()`
  (`workbench.py:746`). That is `min(10800, max_hold_hard - 60)`, which is
  7140 s by default, plus a 900 s stall limit.
- The lock is taken before the build-gate wait (`workbench.py:358-365`), so
  time spent queueing for one of the 8 heavy slots also counts toward the
  lock's age.

The module docstring (`:39-45`) still justifies 2100 s as "comfortably longer
than `build_kilnfw`'s own 1800s subprocess timeout". That stopped being true
when the 3 h progress-based ceiling landed.

After a force reclaim:

- the waiter builds the same directory as the still-running holder (the
  failure `build_lock` exists to prevent);
- the holder's token-checked release then correctly leaves the waiter's lock
  alone;
- but the holder can also race its own release against the reclaim. The
  holder reads its own text, the reclaimer removes it, a third waiter creates
  a lock, and the holder's `_remove_retry` deletes that third lock
  (`:285-286`, a read-then-remove TOCTOU). That is the cascade from BUILDFX3
  MED-1 again, on this path.

Before BUILDFX4 the age path was the only way to recover from a dead holder,
so it had a reason to exist. Now that the lock records a PID plus start time
and the Job object kills orphans, a live holder can be identified with
certainty, and a live holder should never be reclaimed by age.

Reproduced (`repro_age.py`, scratch): holder thread inside the lock, lock
mtime set back 2200 s, a second `build_lock` with default `stale_after` was
**inside after 0.01 s while the first holder was still inside**.

Fix:

- Only take the age path when the content cannot be judged. That means:
  - the content is unparseable (empty file from a holder that died between
    create and write);
  - the content is in the legacy format.
- Never take it when `_pid_info` says the recorded PID is alive with a
  matching start time.
- If an age backstop for a wedged live holder is still wanted, add a
  heartbeat. A holder thread can `os.utime` the lock every ~60 s, and age
  then means "no heartbeat for N minutes", not "acquired N minutes ago".
- The heartbeat also covers a live server whose release leaked the lock.
  `_read_holder` returns None on any OSError (`:218`), so the release at
  `:285` skips the remove. With the age path restricted, such a leak would
  otherwise last until the server restarts.
- Update the docstring.

## LOW

### LOW-1 A reclaim that cannot finish busy-spins and ignores `wait_timeout`

`buildlock.py:272-274`. After `_try_reclaim` the loop does `continue`, which
skips both the `wait_timeout` check (`:275-277`) and the `poll_interval`
sleep (`:278`). `_try_reclaim` returns without removing in several cases:

- the sidecar exists and is younger than 30 s;
- the sidecar `os.open` fails with an `OSError` other than `FileExistsError`;
- `_remove_retry(lock_path)` gives up after 2 s.

In each case the waiter re-judges the same dead lock and calls `_try_reclaim`
again at once.

Reproduced:

- `repro_spin.py`: a dead-PID lock plus a fresh sidecar (a reclaimer killed
  inside `_try_reclaim`). `build_lock(wait_timeout=1.0)` returned only after
  **30.0 s**, after **49 889** `_try_reclaim` calls and **26.6 s of CPU**.
  Every waiter on the lock spins like this.
- `repro_undeletable.py`: a dead-PID lock held open by another process, so
  the delete fails with a sharing violation. `wait_timeout=1.0` returned after
  **14.1 s**, the whole time the file stayed pinned. A persistently
  undeletable lock means an unbounded wait. The docstring promises "never an
  unbounded hang".

Fix:

- Run the `wait_timeout` check on every pass, including after a reclaim
  attempt.
- Sleep a short interval (for example `min(poll_interval, 0.05)`) when
  `_try_reclaim` did not remove the lock. It can return a bool for this.

### LOW-2 The stale-sidecar break is check-then-act and can let two reclaimers in

`buildlock.py:194-198`. `getmtime(sidecar)` judged stale, then
`os.remove(sidecar)`, with nothing tying the judged file to the removed one.
This repeats the pattern BUILDFX3 MED-1 fixed for the lock itself.

After a reclaimer dies holding the sidecar:

1. W1 and W2 both judge the sidecar stale.
2. W2 removes it, and reclaimer R `O_EXCL`-creates a fresh sidecar.
3. W1's `os.remove` deletes R's live sidecar.
4. W3 then creates its own sidecar.

R and W3 are now both inside `_try_reclaim`. Each can pass `current ==
judged`, then:

1. R removes the dead lock;
2. a new holder N creates its lock;
3. W3 removes N's lock;

and two holders build.

Demonstrated by a deterministic interleave (`repro_sidecar_break.py`): R's
live sidecar was deleted and W3 then held a sidecar while R was still inside.

This needs a reclaimer to have died inside a window of a few milliseconds
first, so it is rare.

Fix: break a stale sidecar the same way the lock is reclaimed. Either:

- rename it to a unique name and check the renamed file's mtime; or
- write a token into the sidecar and remove it only if the content you
  judged stale is still what is there.

The simplest option is to `os.replace` the stale sidecar onto a uniquely
named tombstone and treat any failure as contention.

### LOW-3 Stale-sidecar recovery is untested (negtest MISSED)

Mutation M05 (`if time.time() - os.path.getmtime(sidecar) > ...` changed to
`if False:`) is **MISSED**. No test plants an old sidecar and checks that a
waiter eventually reclaims. With M05 in place, a reclaimer that died holding
the sidecar wedges every waiter on that lock forever. Because of LOW-1 that
wedge is also a 100%-CPU spin that `wait_timeout` never ends.

Fix: a test that plants a dead-PID lock and a sidecar with mtime 60 s old,
then asserts that `build_lock(wait_timeout=5)` acquires. Once LOW-1 is fixed,
add a companion test: with a fresh sidecar, `build_lock(wait_timeout=0.5)`
raises `BuildLockTimeout` within about 1 s.

Two more mutations were caught only weakly (see the mutation table):

- M17 (`PermissionError` no longer treated as contention) was caught only by an
  escaping exception.
- M18 (no sidecar serialization) was caught only through a fixed-path artifact.
  The stress test missed it in this run.

## Notes (no action required)

- **Job assignment happens after the child starts** (`workbench.py:203-204`,
  after `Popen`). A grandchild spawned before `AssignProcessToJobObject`
  would escape the job. PowerShell takes hundreds of milliseconds to start
  before it can spawn idf.py, and assignment follows `Popen` within
  microseconds, so this window is theoretical. Closing it fully needs
  `CREATE_SUSPENDED` plus `ResumeThread`, which `subprocess` does not expose.
- **Job assignment failure is silent.** `assign_to_kill_on_close_job`
  returns False and `_run` discards the result, so a box where assignment
  fails silently gets the pre-BUILDFX4 orphan behaviour. A one-line note in
  the run summary would make that visible.
- **Behaviour change:** `run_pctools_tests` and `run_repo_checks` also go
  through `_run`, so a server restart now kills an in-flight test or check run
  too. That is consistent with the intent and worth knowing.
- **Handles:** `_pid_info` and `assign_to_kill_on_close_job` close every
  `OpenProcess` handle in `finally`. A failed `SetInformationJobObject` closes
  the job. The job handle is created non-inheritable (NULL security
  attributes), so no child keeps the job alive past the server. No leak
  found.
- **Mixed versions:** an MCP server process still running pre-BUILDFX4 code
  writes legacy `<pid> <time>` locks and releases unconditionally. Until
  both servers are restarted, the old process can still delete a new holder's
  lock.
- **PID reuse by a process `_pid_info` cannot open** (`OpenProcess` access
  denied) reads as alive with an unknown start time, so it falls back to the
  age path. After the MED-1 fix it would rely on the heartbeat instead.
  `PROCESS_QUERY_LIMITED_INFORMATION` succeeds on almost every process, so
  this is rare.

## Mutation table

`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_buildlock.py
tests/test_flash_board_pinning.py" -Mutations <scratch>\muts.json`. The
baseline passed (52 tests). Every CAUGHT below was checked in its log: the
mutant imported and ran, and the verdict line is a `FAILED <test>` from an
assertion except where marked, not a collection
or import error.

| # | Mutation | Verdict | Failing test(s) and assertion |
|---|---|---|---|
| M01 | sidecar opened without `O_EXCL` | CAUGHT | stress test `11 != 0 : two waiters held the lock at once`; `reclaimed without owning the sidecar` |
| M02 | drop `current != judged` | CAUGHT | `test_reclaim_requires_identical_content_even_if_new_holder_also_dead` (448c511f2's test) |
| M03 | drop the liveness re-judge inside the sidecar | CAUGHT | `test_reclaim_leaves_a_lock_replaced_since_it_was_judged` |
| M04 | release removes the lock unconditionally | CAUGHT | `test_release_does_not_remove_a_lock_it_no_longer_owns` |
| M05 | stale sidecar never broken | **MISSED** | none, 52 passed (LOW-3) |
| M06 | `_START_SLACK_S = 1e9` | CAUGHT | `test_reused_pid_started_after_lock_is_reclaimed`, `test_start_within_slack_is_same_process` |
| M07 | PID reuse never detected | CAUGHT | same two tests as M06 |
| M08 | access denied treated as dead | CAUGHT | `PidInfoBranches.test_access_denied_means_alive` |
| M09 | `STILL_ACTIVE` check dropped | CAUGHT | `test_exited_process_with_open_handle_is_dead` (tuple assertion) |
| M10 | `GetExitCodeProcess` failure treated as dead | CAUGHT | `test_exit_code_query_failure_means_alive` |
| M11 | `GetProcessTimes` failure treated as dead | CAUGHT | `test_get_process_times_failure_means_alive_unknown_start` |
| M12 | own start time not recorded (writes 0) | CAUGHT | `test_lock_records_holders_own_process_start` |
| M13 | dead PID not reclaimed | CAUGHT | `identical dead lock not reclaimed` assertion, plus timeouts in two tests |
| M14 | job created without KILL_ON_JOB_CLOSE | CAUGHT | `test_child_dies_when_holder_is_killed` (orphan survived) |
| M15 | `_run` never assigns the job | CAUGHT | `test_child_dies_when_holder_is_killed`, `test_run_assigns_every_child_to_the_job` |
| M16 | `fixture_flash` `<= 0` changed to `< 0` | CAUGHT | `test_zero_byte_image_refused_before_any_openocd_action` |
| M17 | `PermissionError` no longer treated as contention | CAUGHT, **exception only** | stress test failed only through a worker's escaping `PermissionError`, which broke the barrier. No assertion names this behaviour. |
| M18 | per-waiter unique sidecar (no serialization) | CAUGHT, **incidental** | only `test_reclaim_sidecar_held_by_another_blocks_removal`, through its fixed sidecar path. In this run the stress test did **not** detect the missing serialization. |

Caveat on M17 and M18: the 8-process stress test is probabilistic. It caught
M01 with 11 overlaps, but it did not catch M18, whose effect is the same (any
number of reclaimers inside at once). M17 fails only because an exception
escapes, not because of an assertion. Neither weakens the code under review.
If wanted, two cheap additions would make these tests deterministic:

- for M17, a unit test that patches `os.open` to raise `PermissionError` once,
  then asserts that the lock is acquired;
- for M18, an interleave test like `repro_sidecar_break.py`.

## Repro scripts (scratch, not committed)

| Script | Shows |
|---|---|
| `repro_age.py` | MED-1: a live holder older than 2100 s, second holder inside in 0.01 s |
| `repro_spin.py` | LOW-1: a fresh orphan sidecar, `wait_timeout=1` returns at 30 s, 49 889 reclaim calls, 26.6 s CPU |
| `repro_undeletable.py` | LOW-1: a dead lock held open, `wait_timeout=1` returns at 14.1 s (as long as it stays pinned) |
| `repro_sidecar_break.py` | LOW-2: a stale-sidecar break deletes a live sidecar |
