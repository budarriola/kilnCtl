# Review: BUILDFX5 (2c29ae547, c018fd449, 103749263), 2026-10-12

Adversarial review of the BUILDFX5 fixes for `REVIEW_BUILDFX4_2026-10-12.md`
in `tools/PcTools/src/mcpkit/buildlock.py` and
`tools/PcTools/tests/test_buildlock.py`:

- `2c29ae547`: a confirmed-alive holder is never age-reclaimed; no
  `continue` after a reclaim attempt (the timeout check and poll sleep run on
  every pass); stale-sidecar break by rename-aside; release by rename-aside
  with restore; tests.
- `c018fd449`: the spin test raises `AssertionError` after 500 reclaim calls.
- `103749263`: sidecar-break race test; a `pytest.mark.timeout(30)` on the
  test class.

Reviewed in `C:\wt\rvbuildfx5_*` at origin/dev `103749263`.

**Verdict: 0 HIGH, 0 MED, 3 LOW (LOW-3 fixed here), 5 INFO.**

- BUILDFX4 MED-1 is fixed. A token-bearing lock whose PID is alive and whose
  start time matches is never removed by age (`buildlock.py:238-239`).
  Mutation M02 is caught.
- BUILDFX4 LOW-1 is fixed. With the `continue` gone, a declined reclaim falls
  through to the timeout check and the poll sleep (`:305-312`). M01, M09 and
  M13 are caught (after LOW-3's test fix).
- BUILDFX4 LOW-2 is narrowed but not closed. The break no longer deletes a
  live sidecar it judged by a stale stat, but the vacancy between
  rename-aside and restore, and the unconditional sidecar removal in
  `_try_reclaim`'s `finally`, can still let two reclaimers in (LOW-2 below).
- BUILDFX4 LOW-3 is fixed for the stale-sidecar path (M03 caught).
- New: removing the age backstop for live holders means a leaked lock now
  lasts for the life of the MCP server, with no documented escape (LOW-1).

## LOW

### LOW-1 A leaked release now wedges the build directory until the MCP server restarts, and the escape is undocumented

`buildlock.py:328-337` and `:338-342`.

Every contender for a build lock is normally a thread of the one long-running
`kilnctrl` MCP server, so the lock records that server's own PID and start
time. With BUILDFX5, such a lock is "confirmed alive" for the whole life of
the server and is never removed by age. That is right while a build is
running. It is wrong when the release did not happen. The release can fail
in two ways:

- **Rename-aside gives up.** `_release` tries `os.rename(lock_path, aside)`
  400 times and then returns silently (`:336-337`). On this machine those
  400 attempts took 7.2 s, because Windows rounds a 5 ms sleep up to about
  15 ms. Any process that holds the lock file open without
  `FILE_SHARE_DELETE` for that long blocks the rename: a waiter's
  `_read_holder` (Python's `open()` does not share delete), or AV or the
  indexer.
- **The one-shot read of the aside file fails.** If `_read_holder(aside)`
  hits a transient `OSError` it returns None, so `None == my_text` is False
  and `_release` restores its own lock to `lock_path` (`:338-342`). The lock
  is then leaked.

After either failure, every later `_run_locked` call on that directory waits
its full `wait_timeout` and returns `FAILED (lock contention)`. For
`build_kilnfw` that wait is 7230 s. This lasts until the server restarts.
Before BUILDFX5, the 2100 s age path recovered from this. BUILDFX4 MED-1
predicted this outcome and suggested a heartbeat, which was not adopted.

The same applies to a holder thread that hangs forever. The subprocess is
bounded by `_run`'s ceiling and stall kill, and the gate wait has its own
bound. A Python-level hang in the holder thread is not bounded by anything.

Reproduced (`repro_leak.py`, scratch): with `os.rename` of the lock failing,
`_release` returned after 7.19 s and left the lock in place. A second
`build_lock(stale_after=10)` in the same process, with the lock's mtime set
back 9000 s, timed out naming the holder `"<own pid> <own start> <token>"`.

There are two escapes: restart the MCP server (the PID is then dead and the
lock is reclaimed at once), or delete
`%TEMP%\kilnctl-builds\locks\<key>.lock`. Neither is documented. The
`BuildLockTimeout` message gives the holder's content but not the lock path.

Fix (any one of these is enough; the first is the cheapest):

- Name the lock file path in `BuildLockTimeout`. When the holder PID is the
  current process and no thread of this process holds the lock (track this
  with an in-process set of held keys), say so: "leaked by this server;
  restart it or delete <path>".
- Retry `_read_holder(aside)` a few times before treating the aside file as
  foreign. When the rename-aside gives up, log it loudly instead of returning
  silently.
- Adopt BUILDFX4's heartbeat suggestion: `os.utime` the lock every ~60 s
  from a holder thread, and allow the age path for a confirmed-alive holder
  only when the heartbeat is older than N minutes.

### LOW-2 The sidecar break can still admit two reclaimers (vacancy window and unconditional `finally` removal)

`buildlock.py:204-215` and `:243-244`.

BUILDFX5 breaks a stale sidecar by renaming it to a unique name, re-checking
the age of the renamed file, and renaming it back if it turns out to be
fresh. While the sidecar is renamed aside, its path is vacant. Interleaving:

1. W1 and W2 both stat the old, abandoned sidecar S0 as stale.
2. W1 renames S0 aside.
3. Reclaimer R `O_EXCL`-creates a fresh sidecar S_R and enters
   `_try_reclaim`.
4. W2's rename now grabs S_R. S_R reads as fresh, so W2 tries to restore it.
5. Between W2's rename and its restore, W3 `O_EXCL`-creates S_W3.
6. W2's restore fails (the destination exists), so W2 deletes S_R.
7. R and W3 are both inside the critical section.

Separately, `_try_reclaim`'s `finally` removes the sidecar path without
checking that the file is still its own (`:244`). If R's sidecar was broken
and replaced, R's exit deletes W3's live sidecar, and a fourth waiter can
enter. This is the same token-less pattern that BUILDFX5 fixed for the lock
itself. It is also reachable without any rename race whenever a live
reclaimer stalls for more than 30 s inside `_try_reclaim` (for example, a
suspended process).

Reproduced (`repro_sidecar_vacancy.py`, scratch, deterministic mocks): after
the break, the sidecar was owned by W3 while R was still inside. R's
`finally` then removed W3's sidecar.

Two reclaimers inside do not by themselves break the lock. The
`current == judged` re-read stops the second one, unless it read the dead
content before the first one removed it and a new holder then created a
lock. Reaching a double holder therefore needs:

- a reclaimer to die holding the sidecar;
- a three-way microsecond race;
- then the BUILDFX3 MED-1 interleave.

This is rarer than BUILDFX4 LOW-2. It stays LOW because it is the same class.

Fix: write a token into the sidecar (as the lock does). In `finally`,
remove the sidecar only when it still holds that token, using the same
rename-aside-and-verify that `_release` uses. Without OS locking, a Windows
compare-and-delete always has a vacancy window. If the residual is accepted,
document it next to `_release`'s note.

### LOW-3 The spin test hung instead of failing; two tests failed only by an escaping exception (fixed in this review)

`test_buildlock.py`, class `BuildLockReviewFixes`.

- **M01 was MISSED in the first run.** M01 restores the BUILDFX4 `continue`.
  The 500-call guard that `c018fd449` added lives only in
  `test_declined_reclaim_does_not_spin_and_honours_timeout`. The spin first
  hits `test_alive_holder_old_lock_is_never_age_reclaimed`, which runs
  earlier and has no guard. There pytest-timeout fired. On Windows,
  pytest-timeout's `thread` method dumps stacks and `os._exit`s the whole
  pytest process, so no `FAILED` line is printed and every later test is
  lost. M13 (wait_timeout ignored on the reclaim path) hung the same way.
- **M03 and M10 were caught only by an escaping `BuildLockTimeout`.**
  `test_stale_sidecar_is_broken_and_live_one_is_kept` (M03) and the new
  unknown-start test (M10) failed only through that exception, not through an
  assertion.

Fixed in `a2f9e21bc` and `74104eba4`:

- `setUp` wraps `bl._try_reclaim` for every test in the class. The wrapper
  raises `AssertionError("build_lock kept reclaiming ...")` after 2000 calls
  or 10 s from the first call. A spin or an ignored timeout therefore fails
  the test that hits it, well inside the 30 s pytest-timeout. Pytest-timeout
  stays as a backstop.
- The stale-sidecar test turns a `BuildLockTimeout` into `self.fail(...)`,
  checks the sidecar before its cleanup, and cleans up both files.
- New `test_unknown_start_time_holder_is_still_age_reclaimed` pins the
  documented rule that alive with an unknown start time is not confirmed
  alive (M10).
- New `test_release_restore_never_overwrites_a_newer_lock` (M07, M08).

## INFO

- **I-1 Liveness on Windows (question 1).**
  - The check is robust for its purpose. `GetProcessTimes` returns the
    kernel's creation FILETIME, which is fixed at process creation, so NTP or
    manual clock steps do not move it. The holder and the judge read the same
    value.
  - `int()` truncation loses less than 1 s, inside the 2 s slack.
  - A reused PID is misjudged as alive only if the original holder lived less
    than 2 s and its PID was reused within that window.
  - `PROCESS_QUERY_LIMITED_INFORMATION` works across sessions and against
    elevated processes of the same user.
  - The lock directory is per-user `%TEMP%`, so a holder belonging to another
    user is not a case that arises.
  - Access denied reads as alive with an unknown start time. That is not
    confirmed alive, so the age path still applies (now pinned by a test).
  - On POSIX the start time is always None, so every live holder is
    age-reclaimable. The repo is Windows-only.
- **I-2 The age path still force-reclaims holders that are alive but not
  confirmable.** These are holders with an unknown start time, holders that
  recorded a start of `0`, and live legacy-format holders (a pre-BUILDFX4
  server process).
  - After 2100 s such a holder is reclaimed.
  - Under `force`, the window between `_try_reclaim`'s re-read (`:235`) and
    `_remove_retry(lock_path)` (`:242`) races that holder's own release: the
    holder renames its lock aside, a third waiter creates a lock, and the
    reclaimer deletes it.
  - This is also the only route into `_release`'s documented residual
    (question 2). Our lock is removed only by a reclaimer, and a confirmed-alive
    holder is never reclaimed, so the claim in `_release`'s docstring holds.
- **I-3 Rename semantics (question 2).** On Windows, `os.rename` is
  `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING`, so it fails with
  `FileExistsError` when the destination exists. Neither the restore in
  `_release` (`:342`) nor the restore in `_break_stale_sidecar` (`:213`) can
  overwrite a newer file. This is now pinned by
  `test_release_restore_never_overwrites_a_newer_lock`: M07 (`os.replace`)
  is caught. On POSIX both restores would clobber. Windows-only.
- **I-4 Callers (question 4).**
  - `build_lock` has one production caller, `workbench._run_locked`
    (`workbench.py:358`). It always passes a bounded
    `wait_timeout = timeout + 90`.
  - The flash paths do not take this lock. `test_flash_board_pinning.py` does
    not import it, and `flash_firmware`/`fixture_flash` serialize through
    other means.
  - No caller waits forever. The only behaviour change is intended: a waiter
    behind a live holder older than 35 minutes used to reclaim and build into
    the same directory. It now waits up to its `wait_timeout` (7230 s for
    `build_kilnfw`) and reports `FAILED (lock contention)`.
- **I-5 Cost while waiting on a live old lock.** Every poll creates and
  deletes the sidecar and opens the holder process twice. That is cheap at
  1 s polls. Checking `_holder_confirmed_alive(judged)` before taking the
  sidecar would skip it.

## Mutation table

`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_buildlock.py
tests/test_workbench_contention.py tests/test_build_kilnfw_artifact_gate.py"
-Mutations <scratch>\rvbuildfx5\muts.json`. Run 1 was at `103749263`, as
landed. Run 2 was at `a2f9e21bc`, after LOW-3's first test fix. M03 and M10
were rerun at `74104eba4`. The baseline passed in every run, and the real tree
was unchanged. Every final CAUGHT was checked in its log: a named test failed
with an `AssertionError`, except where marked.

| # | Mutation | Run 1 (as landed) | Final | Failing test and assertion |
|---|---|---|---|---|
| M01 | `continue` after `_try_reclaim` (BUILDFX4 spin) | **MISSED** (whole run killed by pytest-timeout, no FAILED line) | CAUGHT | `test_alive_holder_old_lock_is_never_age_reclaimed`: `build_lock kept reclaiming (2001 calls in 4.7s)`; `test_declined_reclaim_...` |
| M02 | drop the `_holder_confirmed_alive` refusal | CAUGHT | CAUGHT | `test_alive_holder_old_lock...`: `BuildLockTimeout not raised` |
| M03 | stale sidecar never broken | CAUGHT (escaping `BuildLockTimeout` only) | CAUGHT | `test_stale_sidecar_...`: `stale sidecar never broken: dead lock not reclaimed` |
| M04 | grabbed live sidecar deleted, not restored | CAUGHT | CAUGHT | `test_sidecar_break_restores_...`: `deleted a live sidecar` |
| M05 | no re-check of the grabbed sidecar's age | CAUGHT | CAUGHT | same test, same assertion |
| M06 | release deletes the aside file regardless of token | CAUGHT | CAUGHT | `test_release_does_not_remove_a_lock_it_no_longer_owns`: `released a lock owned by another holder`; `test_release_puts_back_...` |
| M07 | restore with `os.replace` (overwrites a newer lock) | **MISSED** | CAUGHT | new `test_release_restore_never_overwrites_a_newer_lock`: `'1 2 tokenA' != '3 4 tokenB'` |
| M08 | failed restore leaks the `.rel.` aside file | **MISSED** | CAUGHT | same new test: `Lists differ: ['...lock.rel.4d7c...'] != []` |
| M09 | poll sleep removed | CAUGHT | CAUGHT | `test_declined_reclaim_...`: `419 not less than 20` |
| M10 | unknown start time counts as confirmed alive | **MISSED** | CAUGHT | new `test_unknown_start_time_holder_is_still_age_reclaimed`: `old lock of a holder with unknown start time was not age-reclaimed` |
| M11 | tokenless (legacy) lock can be confirmed alive | MISSED | MISSED | Equivalent in practice. A legacy second field is the acquire time, so it matches the PID's start within 2 s only if the lock was taken in the holder's first 2 s. Not worth a test. |
| M12 | rename-aside in release tries once | CAUGHT | CAUGHT, **exception only** | stress test `test_eight_processes_dead_pid_lock_never_overlap`: `BrokenBarrierError` after a leaked lock, no assertion. Same weakness as BUILDFX4 M17. |
| M13 | `wait_timeout` not checked when a reclaim was attempted | **MISSED** (hang, run killed) | CAUGHT | `test_alive_holder_old_lock...`: `build_lock kept reclaiming (183 calls in 10.0s)` |

## Repro scripts (scratch, not committed)

| Script | Shows |
|---|---|
| `repro_leak.py` | LOW-1: a release whose rename keeps failing returns after 7.2 s and leaves the lock. A later caller in the same process times out against its own live PID even with the lock aged 9000 s. |
| `repro_sidecar_vacancy.py` | LOW-2: a break that grabbed a live sidecar loses it to a third reclaimer during the vacancy. R's `finally` then deletes that third sidecar. |
