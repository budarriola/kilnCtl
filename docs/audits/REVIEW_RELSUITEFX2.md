# REVIEW_RELSUITEFX2: review of the REVIEW_RELSUITEFX fixes (e0d995101, b93c538f4, 11fd07098)

Date: 2026-10-13. Scope: `tools/PcTools/src/kilnctrl/bench_test/cases_release.py`,
`bench_test/runner.py`, `mcp_server_bench_test.py` and their tests
(`tests/test_bench_test_cases_release.py`, `tests/test_bench_test_start_jobs.py`),
at `11fd07098`. No board access. Paths are relative to
`tools/PcTools/src/kilnctrl/` unless they start with `tools/` or `tests/`.

Counts: 0 HIGH, 1 MED, 5 LOW, 4 INFO.

The fixes for MED-A (every stage outcome cancels then clears), MED-B (board
identity before heat), LOW-1 (runner confirm gate), LOW-3 (timeout bound,
re-baseline) and INFO-2 (release `allow_heat` default off) are in place and
mostly tested: 25 of 27 mutations were caught (table below). The remaining
problems are about which board each phase talks to, and about states the D01
cleanup does not verify.

## Findings

### MED-1: the trip and OTA phases do not use the host the run identified

`ctx["host"]` is resolved once in `mcp_server_bench_test.py` (`_ota_resolve_host_with_source(host)`,
around :135) and used by A01/F01 snapshots and by `_board_identity_problem`
(`bench_test/cases_release.py:639-654`). The B01 and D01 calls never pass it:

- B01: `trip_fn(confirm=True)` (`:484`); `safety_test_trip` takes `host=None`
  (`mcp_server_safety.py:380`) and resolves its own.
- D01: `update_status()` (`:729`, `:765`), `update_check(...)` (`:736`),
  `update_stage_release(...)` (`:747`), `update_fetch_cancel(confirm=True)`,
  `update_stage_clear(confirm=True)` (`:779`) and `update_install_staged(confirm=True)`
  (`:796`) all take `host=None` (`mcp_server_update.py:76, 241, 340, 373, 524, 563`).

With `host=None` both sides resolve the same way, so the default run is
consistent apart from a per-call re-resolution. With an explicit
`bench_test_run(host=...)`, the release suite baselines and identity-checks
one board, then trips, stages and can install firmware on whatever board the
tools resolve by default. D01 and B01 run no identity check of their own, so
nothing catches the split. MED-B's fix ("refuse unless the UART board is the
HTTP board") does not cover these phases.

Fix: pass `host=ctx["host"]` to every HTTP tool call in B01 and D01 (and to
`_install_and_verify`'s `install`), and run `_board_identity_problem`-style
UART-vs-HTTP checks before D01's stage and before B01's trip.

### LOW-1: identity check is not pinned to the IP it compared, and runs once

`_board_identity_problem` (`:639-660`) calls `idfn(host)[0]` and discards the
IP the aux helper compared (`mcp_server_aux._board_identity_check` returns
`(mismatch, ip)`; the aux tool pins it with `_pin_host`). The later
`get_zones` and `_check_zone_ceilings` reads re-resolve `ctx["host"]`. For an
IP literal this is moot. For a hostname it is a DNS TOCTOU. The check also
runs once, before the UART profile save and start in C01; nothing re-checks
between the identity read and the heat start.

### LOW-2: B01 passes when the UART diag shows no latched trip

`:499`: `if pre_reason not in (0, None) and (pre_reason != 4 or pre_mask != 0x0008)`.
If `safety_test_trip` reports the test trip (HTTP) but the UART
`safety_get_diag()` shows reason 0 (or an unparsable diag, None), the case
clears and can PASS. That is either a board mismatch (MED-1) or a trip that
never latched, and neither should pass. The fail-safe direction holds (a
board tripped over HTTP but not seen over UART stays tripped), but the
verdict is wrong. Require `pre_reason == 4 and pre_mask == 0x0008` for a PASS;
report 0/None as FAIL or INCONCLUSIVE.

### LOW-3: D01 cleanup does not verify the cancel or the clear

`:768-787`:

- `update_fetch_cancel` is looked up with `getattr(srv, ..., None)`; when it
  is absent the cancel is silently skipped and the clear runs against a
  download that may still be writing.
- A cancel that fails or raises is only recorded in `obs`; it neither taints
  the run nor changes the verdict.
- After `update_stage_clear` there is no `update_status()` read-back. A fetch
  that was not cancelled can re-write `stage` after the clear, and the run
  reports a clean board.

Fix: require the cancel tool, taint on a non-ok cancel, and re-read status
after the clear (staged=False, busy=False, header=none), tainting otherwise.

### LOW-4: an install that raises is labelled "stage raised" and the clear can race it

`_install_and_verify`'s `install(confirm=True)` (`:796`) has no own
`try`. An exception there reaches D01's outer handler (`:759-760`) as
`stage raised ...`, and because `install_ok` is still False, the cleanup
then cancels and clears the stage while the install (or its reboot) may be
in progress. Catch install exceptions separately, report them as install
failures, and treat the board state as unknown (taint, no clear) rather
than clearing mid-install.

### LOW-5: re-baseline takes the first snapshot with a fw_build

`:804-818`: the poll accepts the first `_snapshot` that has
`identity.fw_build`. A snapshot taken in the first 10 s, before the board
starts its reboot, still carries the old build; the case then FAILs
"fw_build unchanged" (a false FAIL, not a false PASS) and stores the
pre-reboot snapshot as the new baseline, so RL-F01 compares against
pre-reboot watermarks. The new baseline is also stored without checking its
`_unreadable` list, so F01 can compare against fields that were never read.
Wait for a build different from `before_build` (or an uptime drop) before
accepting, and keep the baseline None when the accepted snapshot has
unreadable fields that F01 compares.

### INFO-1: timeout growth bound is chosen, not measured, and only loosely pinned

`LINK_TIMEOUT_GROWTH_BOUND = 20` (`:191`) has no cited bench measurement.
`test_link_timeout_growth_bound` asserts `0 < bound <= 50`, base+20 passes
and base+1000 fails, so any value from 20 to 50 passes the test. Mutants
`bound=0` and `bound=100000` were caught; a quiet change to 50 would not be.

### INFO-2: a bool link counter is not tested (mutant M23 MISSED)

`_counter` (`:287-291`) refuses `bool`. Dropping `isinstance(v, bool)` left
every test green: no test feeds `True`/`False` as `crc_errors` or `timeouts`.
The real `get_link_stats()` returns ints, so the guard is defence in depth;
add a one-line test if it is meant to stay.

### INFO-3: mutant M26 is effectively equivalent

M26 replaced `not post.startswith("ok")` in the `dirty` expression with
`False`. Every non-ok re-read string also lacks `staged=False`, so the next
clause still marks it dirty. Not a test gap; the clause is redundant.

### INFO-4: runner confirm gate (REVIEW_RELSUITEFX LOW-1): no bypass found

`runner.py:423` refuses `suite == "release"` without `confirm is True` unless
`dry_run`. Requested case ids are filtered to suite membership, RL ids exist
only in `SUITES["release"]`, `run_suite` forwards `confirm`, and the only
other `runner.run` caller (`mcp_server_ota_matrix`) uses suite `ota`. No
caller invokes a release judge outside the runner. Mutants M14, M15 and M27
were caught.

## Negative test

`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_bench_test_cases_release.py tests/test_bench_test_start_jobs.py" -Mutations mut.json`,
base `11fd07098`. Baseline 96 passed. `real_tree_unchanged: true`. Every
CAUGHT log was checked by hand: each shows `N failed, M passed` with a
pytest assertion failure (no collection or import errors).

| # | Mutation | Verdict |
|---|---|---|
| M01 | identity mismatch ignored | CAUGHT |
| M02 | identity exception ignored | CAUGHT |
| M03 | bench ceiling `>=` | CAUGHT |
| M04 | unmasked zones counted | CAUGHT |
| M05 | identity call dropped from C01 | CAUGHT |
| M06 | `dirty` only on stage_ok | CAUGHT |
| M07 | cancel skipped | CAUGHT |
| M08 | `staged` never set | CAUGHT |
| M09 | clear failure not tainted | CAUGHT |
| M10 | timeout bound 100000 | CAUGHT |
| M11 | timeout bound 0 | CAUGHT |
| M12 | no re-baseline after install | CAUGHT |
| M13 | board gone keeps old baseline | CAUGHT |
| M14 | runner gate truthy `confirm` | CAUGHT |
| M15 | runner gate removed | CAUGHT |
| M16 | bench_test_run release allow_heat default on | CAUGHT |
| M17 | bench_test_start release allow_heat default on | CAUGHT |
| M18 | unreadable link counter passes | CAUGHT |
| M19 | busy precheck dropped | CAUGHT |
| M20 | header precheck dropped | CAUGHT |
| M21 | B01 partial-match `or` -> `and` | CAUGHT |
| M22 | CRC growth ignored | CAUGHT |
| M23 | bool counter accepted | MISSED (INFO-2) |
| M24 | kiln ceiling `<` | CAUGHT |
| M25 | cancel `confirm=False` | CAUGHT |
| M26 | post re-read `startswith("ok")` dropped | MISSED (equivalent, INFO-3) |
| M27 | runner confirm not passed | CAUGHT |
