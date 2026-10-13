# Review: BUILDFX7 (ef20016a5, da59e28ec), 2026-10-12

Adversarial review of buildfx7's fixes for `REVIEW_BUILDFX6_2026-10-12.md`,
all in `tools/PcTools/src/mcpkit/buildlock.py`:

- `ef20016a5`: `_try_reclaim` now deletes through a new `_remove_judged`
  (rename-aside, compare with the judged text, remove on a match, rename back
  on a mismatch) (LOW-1). `_release` prints `LEAKED LOCK?` to stderr when the
  aside stays unreadable (LOW-2). The module docstring lists four residual
  windows (INFO-1/2/3/5). The own-process timeout message points at
  `build_job_status` (INFO-4). Tests are in `BuildLockBuildFx7`.
- `da59e28ec`: fix status lines in the BUILDFX6 review.

Reviewed in `C:\wt\rvbuildfx7_*` at `da59e28ec`. Line numbers below are for
that tree.

**Verdict: 0 HIGH, 0 MED, 2 LOW, 5 INFO.** The fix for BUILDFX6 LOW-1 is
correct. Repro G1, rewritten for rename-aside, now keeps the new holder's
lock, and a second reclaimer that finds the path gone gets
`FileNotFoundError` and stops. On Windows, the restore cannot overwrite a
lock created in the gap, because `os.rename` raises `FileExistsError` there
(checked on this machine, repro R4). One gap remains in the new code. The
restore drops the grabbed file on *any* `OSError`, not only on
`FileExistsError`. In the mismatch case that file is a live holder's lock
(LOW-1). BUILDFX6 LOW-2 is only half addressed. The leak still lasts until
the server restarts, and the new log goes to a file that no tool shows
(LOW-2).

## Answers to the review questions

| Question | Answer |
|---|---|
| Two reclaimers each rename aside. Can the rename-back clobber a lock created in the gap? | No, not on Windows. `os.rename` onto an existing path raises `FileExistsError` (R4). The restore then fails and the grabbed file is deleted. The lock made in the gap survives, and the grabbed holder loses its file. That is documented residual (1) (repro R2). On POSIX, `os.rename` overwrites, so the grabbed file would replace the gap lock instead. It is the same double hold, the other way round. The server only runs on Windows. Mutation M5 (`os.replace`) is MISSED because no test covers a conflict at restore time. |
| Can the rename-back fail and leave the lock lost? | Yes, on more than the documented vacancy. `:340-343` treats every `OSError` from the restore as "a newer lock exists" and deletes the grabbed file. A transient sharing violation, for example an AV or indexer handle on the freshly renamed `.rec.` file, deletes a live holder's lock (LOW-1, repro R1). |
| Can it leave the lock vacant? | Only for the restore instant, as documented. The rename-aside give-up (`:335-336`) leaves the judged lock in place, and the next poll retries. |
| `os.rename` vs `os.replace` | `os.rename` is the right call on every restore path. `_break_stale_sidecar`, `_release_sidecar`, `_release` and `_remove_judged` all rely on it refusing to overwrite. |
| Aside file leak on a crash | Yes. A crash between the rename-aside and the remove or restore leaves a `.rec.<uuid>` file forever. Nothing sweeps `.rec.`, `.rel.` or `.dead.` files (R5). If the reclaim was on its normal (match) path, the lock path is left vacant and nothing blocks. If it was on the mismatch path, a live holder's lock is lost (INFO-2). |
| G1-G5 against the fix | G1 as written mocks `os.remove` on `lock_path`. The new code never calls that, so G1 now prints "False" for a reason unrelated to the race. Re-expressed for rename-aside (G1', the fixer's own test scenario), the new holder's lock survives: **fixed**. G2: the lock is still leaked, and stderr now has the line: **partly fixed** (LOW-2). G3, G4 and G5 show the same behavior as before. All three are now documented as residuals (1) and (2). |
| Is leak-until-restart acceptable? | Not as it stands (LOW-2). |
| Is stderr visible from the MCP server? | Only in a file. `mcp_servers.ps1:128-133` starts the server with `-RedirectStandardError "$log.err"`, so the line goes to `tools/PcTools/logs/<name>-mcp.log.err` in the main tree. No MCP tool result, health check or `kiln_help()` output shows it. |

## LOW

### LOW-1 `_remove_judged` deletes a live holder's lock when the restore fails for any reason

`buildlock.py:337-343`.

```python
    if _read_holder(aside) == judged:
        _remove_retry(aside)
        return
    try:
        os.rename(aside, lock_path)
    except OSError:
        _remove_retry(aside)
```

The mismatch branch runs only when the file at `lock_path` is no longer the
judged one. By then it is a new holder's live lock. The restore is a single
attempt. Any `OSError` gets the treatment that is meant for
`FileExistsError`: the grabbed lock is deleted. That includes a sharing
violation, which the rename-aside loop just above (`:327-334`) already
treats as transient and retries 400 times. The new holder keeps building
without a lock, and the next waiter acquires beside it. Two concurrent
builds in one directory is exactly what this module exists to prevent.

Repro R1 (deterministic): `lock_path` holds `"1 0 H"`, and the judged text
is a dead lock. The restore rename raises `PermissionError(32)` once. Result:
`lock_path` is gone and no aside is left.

Reaching it needs two rare things together: the mismatch branch, which needs
a sidecar eviction or a concurrent release by an age-reclaimed unconfirmed
holder, and a third-party handle on the aside. So this is LOW. The
`_remove_judged` docstring also says the only residual is "the microsecond
restore vacancy", which this contradicts. `_release` (`:454-457`),
`_release_sidecar` (`:277-280`) and `_break_stale_sidecar` (`:252-255`) have
the same one-shot restore. In `_release` and `_break_stale_sidecar`, the
grabbed file is also usually live when the restore runs.

Fix: give the restore its own bounded retry. Drop the grabbed file only on
`FileExistsError`, and retry other `OSError`s like the rename-aside loop
does:

```python
    for _ in range(400):
        try:
            os.rename(aside, lock_path)
            return
        except FileExistsError:
            break
        except OSError:
            time.sleep(0.005)
    _remove_retry(aside)
```

Factor this into one `_restore(aside, path)` helper and use it on all four
restore sites. If the retries give up, print a stderr line, as `_release`
does for its own give-up.

### LOW-2 BUILDFX6 LOW-2: the leak is now logged, but it is still unbounded and nobody sees the log

`buildlock.py:449-453`.

The fix adds the `LEAKED LOCK?` line, and the lock is still restored. For a
confirmed-alive server, that restored lock is never reclaimed. Every later
`build_lock` on that directory in this process waits the full
`wait_timeout` and then fails. For `build_kilnfw` that is
`timeout + 90 s`, and `timeout` is the build ceiling of up to 3 h
(`workbench.py:354-355`, `:746`). It fails again on every retry, until
someone reads the timeout message and restarts the server. The stderr line
goes to `tools/PcTools/logs/kilnctrl-mcp.log.err` (see the table above),
which no agent reads. The new message therefore adds nothing for the person
who hits the stall. The `BuildLockTimeout` own-process note is what actually
reaches them, and it already existed.

The trigger is rare: the aside must stay unreadable for more than 80 ms
after the rename. Even so, a 3 h stall per attempt is a poor way to fail,
and a bounded fallback is cheap and safe, because the decision can be made
by exact token match:

- On this path, record `(lock_path, my_text)` in a module-level set,
  `_maybe_leaked`.
- In `build_lock`'s contention branch, if `_read_holder(lock_path)` equals a
  recorded `my_text` for this path, call `_release(lock_path, that_text)`
  again, drop the entry, and retry at once. `my_text` carries a
  per-acquire uuid, so this can only ever remove that lock.
- Optionally, sweep `_maybe_leaked` from the next `_release` in the
  process.

This also covers the `_release` rename give-up path (`:435-439`).

## INFO

### INFO-1 `_remove_judged` matches an unreadable aside when the judged text is None

`buildlock.py:337`. On the age path, `judged` can be None: an empty lock left
by a holder that was killed between `O_EXCL` and `write`. `_read_holder`
returns None for an empty file and also for an unreadable one, so
`None == None` counts as a match. If that stale empty file has meanwhile been
replaced by a new holder's lock that is still empty (between its `O_EXCL`
and its `write`), the new lock is deleted (repro R3). `_release` treats an
unreadable aside as foreign. `_remove_judged` treats it as its own. Getting
there needs a prior remover of the stale file, which means a sidecar
eviction, so this is INFO.

Do not "fix" it by refusing None. The empty-lock reclaim is how the
docstring keeps its promise that "a lock created by a process killed before
it finished the write is still reclaimable". A safe narrowing is this: when
`judged is None`, also require the aside's mtime to be older than
`stale_after`. A rename keeps the mtime, so a fresh new lock fails that test.
Note that **no test covers the empty-stale-lock reclaim at all**: mutation
M9 (never match when `judged is None`) makes such a lock block every waiter
forever, and it is MISSED. Add a test that ages an empty lock past
`stale_after` and acquires it.

### INFO-2 Aside files are never swept

A crash, or a `KeyboardInterrupt` in an in-process thread, between a
rename-aside and its remove or restore leaves `<lock>.rec.<uuid>`,
`.rel.<uuid>` or `.dead.<uuid>` in `%TEMP%\kilnctl-builds\locks` forever
(R5). This is clutter only, except on the mismatch path, where the file left
behind is a live holder's lock. Server crashes mid-reclaim are rare enough
to accept this. An mtime-based sweep of asides older than an hour in
`_lock_dir()` would make it tidy.

### INFO-3 The `LEAKED LOCK?` line says "restored" before the restore runs

`buildlock.py:449-457`. The line is printed before `os.rename(aside,
lock_path)`. If that restore fails, the aside is deleted, and the log says
the opposite of what happened (R6). Print after the restore, with one
wording for each outcome.

### INFO-4 The own-process timeout message is hard to act on

`buildlock.py:103-110` renders as "...(PID and start time match): (another
thread of this server may be running a build: check build_job_status first)
if no build is running here...". A colon is followed by a parenthesis and
then a lowercase "if". The advice also cannot be followed as given.
`build_job_status` needs a job id (`build_jobs.py:202`) and cannot list
jobs. The agent that times out does not have the other caller's id. A
synchronous `build_kilnfw` from another client runs on a server thread and
has no job at all. The message still ends by suggesting the file be deleted.
Suggested wording: "The holder is THIS server process. Another tool call in
this server may still be building this directory; deleting the lock while it
runs allows two concurrent builds. If no build is running here, the lock was
leaked: restart the MCP server or delete <path>." The in-process held-keys
registry from BUILDFX5 LOW-1 (and LOW-2 above) would let the message say
which case it is. Mutation M8 (hint removed) is MISSED, because no test pins
this wording.

### INFO-5 Docstring accuracy

Module docstring `:42-52`:

- (1) is accurate as far as it goes. It omits the transient-restore deletion
  (LOW-1). "All need a prior eviction or reclaim of the releaser's own file"
  also covers `_remove_judged`'s mismatch branch only loosely: that branch
  is also reached when an age-reclaimed holder that is alive but unconfirmed
  releases mid-reclaim.
- (2) and (3) are accurate (G5 confirms (2)).
- (4) is accurate, but "logging to stderr" should say where stderr goes
  (`logs/<name>-mcp.log.err`).
- The `_remove_judged` docstring's "Residual: the microsecond restore
  vacancy" is incomplete (LOW-1, INFO-1, INFO-2).

The test file's new `with` statements (`test_buildlock.py:922`, `:944`) lost
their backslash continuation and carry a run of spaces mid-line. They are
valid Python but read oddly.

## Test quality

The three `BuildLockBuildFx7` tests use the real filesystem (`%TEMP%` lock
directory, unique names per test) and inject faults by patching
`os.rename`, `_read_holder` and `time.sleep`. They do not depend on timing.
The class passed 15 out of 15 consecutive runs on a loaded machine, and the
whole file passed the negtest baseline twice. Patching `bl.os.rename` patches
the global `os.rename`, which is safe here only because none of these tests
start threads.

Gaps (from the mutation run): the rename-aside retry in `_remove_judged` is
untested (M4, see the table). Restore-conflict semantics (M5), the INFO-4
wording (M8) and the empty-stale-lock reclaim (M9, INFO-1) are untested too.

## Repros

These are scratch scripts and are not committed. `repro_fx7.py` asserts the
current behavior by printing it:

- G1': the fixer's scenario. The new holder's lock survives: fixed.
- R1: a transient restore failure deletes the live lock (LOW-1).
- R2: a vacancy grab at restore: W holds, H's file is gone (documented
  residual (1)).
- R3: `judged=None` deletes a new holder's empty lock (INFO-1).
- R4: on Windows, `os.rename` onto an existing file raises
  `FileExistsError`.
- R5: an interrupted `_remove_judged` leaves an orphan `.rec.` file
  (INFO-2).
- R6: the `LEAKED LOCK?` line says "restored" when the restore failed and
  the file was deleted (INFO-3).

## Negative tests

`tools\negtest.ps1 -Preset pytest -PresetArg tests/test_buildlock.py`
against `da59e28ec`. The fixer's M1-M3 were rerun, plus six new mutations.
Baseline PASS, real tree unchanged, copies removed. Every CAUGHT was checked
in its log to be an `AssertionError` from a named test, not an import or
collection error. (A first run died at M2 because the machine's disk was
full, `No space left on device`. Its copy was removed and the whole run was
repeated once space was free. Only the second run counts.)

| ID | Mutation (`buildlock.py`) | Verdict | Caught by (AssertionError) |
|---|---|---|---|
| M1 | `_try_reclaim` deletes by path again (`_remove_retry(lock_path)`) | CAUGHT | `test_reclaim_retry_never_deletes_a_new_holders_lock` (`None != '1 0 H'`) |
| M2 | `_remove_judged` compare always true | CAUGHT | same |
| M3 | No `LEAKED LOCK?` log | CAUGHT | `test_release_logs_when_aside_stays_unreadable` (`'LEAKED LOCK' not found in ''`) |
| M4 | `_remove_judged` rename-aside not retried (`range(1)`) | MISSED | no test. The fixer's G1 test passes either way, because a give-up also leaves H's lock alone |
| M5 | `_remove_judged` restores with `os.replace` (overwrites) | MISSED | no test covers a conflict at restore |
| M6 | `_remove_judged` mismatch deletes instead of restoring | CAUGHT | `test_reclaim_retry_never_deletes_a_new_holders_lock` |
| M7 | Leak line printed to stdout | CAUGHT | `test_release_logs_when_aside_stays_unreadable` |
| M8 | INFO-4 hint dropped from the message | MISSED | no test pins the wording |
| M9 | `_remove_judged` never matches when `judged is None` (empty stale lock never reclaimed) | MISSED | no test ages an empty lock (INFO-1) |

Suggested tests:

- M4: the first rename-aside raises `PermissionError`, and the judged lock
  must still be removed.
- M5: create W's lock at restore time, and assert that W's text is still at
  the path.
- M9: age an empty lock past `stale_after`, and assert that `build_lock`
  acquires it.
- LOW-1 regression: the restore raises `PermissionError` once, and H's lock
  must survive.

## Fix status (buildfx8)

LOW-1, LOW-2 and INFO-1 to INFO-5 are fixed in `buildlock.py` / `test_buildlock.py`
(`BuildLockBuildFx8`), commits below on origin/dev:

- LOW-1: shared `_put_back` (retries any non-`FileExistsError`; on
  `FileExistsError` deletes the moved file only when its text equals the
  newcomer's, else leaves it as an aside); used at all four put-back sites.
- LOW-2: `_maybe_leaked` records leaked own lock texts; the next contention frees
  one by exact text match.
- INFO-1: a `judged is None` match also needs the aside older than `stale_after`.
- INFO-2: `build_lock` sweeps `.rec./.rel./.dead.` asides older than `stale_after`.
- INFO-3: "restored" is logged only after a successful put-back.
- INFO-4: reviewer's wording, pinned by a test. INFO-5: docstring and test
  continuations fixed.
- M4, M5, M8, M9 and one mutant per new fix are CAUGHT by AssertionError.
  Note: `test_release_restore_never_overwrites_a_newer_lock` no longer asserts that
  a differing grabbed aside is deleted (it is left for the sweep, per LOW-1).
