# REVIEW_RELSUITEFX: review of the REVIEW_RELSUITE fixes (8652ecaef, 87721270a)

Date: 2026-10-13. Scope: `tools/PcTools/src/kilnctrl/bench_test/cases_release.py`,
`tools/PcTools/src/kilnctrl/mcp_server_bench_test.py`, `tools/release_gates.py`
and their tests (`tests/test_bench_test_cases_release.py`,
`tests/test_bench_test_start_jobs.py`, `tests/test_release_gates.py`), at
`87721270a`. No board access. Paths below are relative to `tools/PcTools/src/kilnctrl/`
unless they start with `tools/` or `tests/`.

Counts: 0 HIGH, 2 MED, 5 LOW, 4 INFO.

## Findings

### MED-A: MED-3 is only partly fixed; a FAILED or UNKNOWN stage result leaves the image staged

`bench_test/cases_release.py:705-724`. RL-D01 sets `staged = True` only when
`update_stage_release` returns a string starting with `ok`, or when it raises.
`mcp_server_update.py` has four non-ok returns that come *after* the download
was started or finished:

- `:422` UNKNOWN: download started, status unreadable.
- `:426` FAILED: download did not finish ok (partial write possible).
- `:430` UNKNOWN: job reported done, but the confirming read failed.
- `:444` FAILED: job reported done, but the read-back disagrees. The bytes are
  in the stage partition.

On all four, RL-D01 returns FAIL with `staged` still False. No clear runs and
the run is not tainted, so a downloaded or half-written image stays in `stage`.
That is the exact outcome MED-3 was meant to prevent.

Fix: once `update_stage_release(confirm=True)` has been invoked, re-read
`update_status` whatever it returned. If `staged=True`, `busy=True` or a header
is present, cancel any in-flight fetch with `update_fetch_cancel` and then call
`update_stage_clear`. Taint the run if the clear fails. Only `:406` and `:419`
are known to be pre-download refusals.

There is no test of a FAILED or UNKNOWN stage result. Negtest M06, which turns
the exception path into `staged = False`, was MISSED: no test makes
`update_stage_release` raise.

### MED-B: MED-5's identity check reads one channel while heat runs on another; a direct signal exists

`bench_test/cases_release.py:546` and `:621-643`. `_board_identity_problem`
reads zone ceilings over HTTP from the ctx host (`zones_http_client.get_zones`).
The heat that follows is saved and started over the UART link:
`H._start_bench_profile` calls `srv._profiles.save` and `srv._profiles.start`
(`bench_test/cases_heat.py:324`, `:335`). Nothing pins the HTTP host to the
board on the UART. When the two disagree, the check verifies one board and a
different board heats. RL-B01 mixes channels in the same way: the trip is
posted over HTTP (`safety_test_trip`), while the diag and clear go over UART
(`:475`, `:487`).

A more direct identity signal already exists. `mcp_server_aux.py:222`
`_board_identity_check` compares the station IP that the UART link reports
against the HTTP host. RL-A01 or RL-C01 should call it, or an equivalent, and
refuse on a mismatch. The ceiling heuristic should stay only as a secondary
target-class check.

The ceiling split itself is a weak signal:

- `top = max(ceilings)` (`:638`) covers every zone, not just the zones in the
  heat mask.
- A bench with any zone ceiling raised above 200 C passes as `kiln` and is
  driven to `KILN_HEAT_TARGET_C`. In the other direction, it is refused as
  `bench`.
- A real kiln whose ceilings are all at or below 200 C passes as `bench`. That
  is low harm, because the bench profile is ambient plus an offset.
- The tests use only 150 and 1300 single-zone configs: no 200/200.1 boundary
  case and no multi-zone case. Negtest M01 (200 changed to 2000) was caught
  only because the kiln fixture uses 1300.

### LOW-1: the release confirm gate lives only in the MCP layer

`mcp_server_bench_test.py:160` and `:322` gate `suite == "release"`.
`bench_test/runner.py:656` `run_suite()` and `BenchTestRunner.run` run the
release suite without a confirm. Nothing calls `run_suite` today, so this is
defence in depth. Moving the gate into the runner, or into the release cases'
ctx, would cover any future entry point. `tools/release_gates.py` only reads
evidence and needs no gate (verified).

### LOW-2: the gate is keyed on the suite name, not on case membership

`mcp_server_bench_test.py:160` and `:322`. Today only `SUITES["release"]`
contains RL-* ids, and the runner refuses ids that are not in the named suite,
so this is sound. However, no test pins that RL-* ids stay out of other suites.
Adding RL-D01 to another suite would bypass both the confirm and the
`ota_install` gate. A registry test asserting that every `RL-` id belongs only
to `release` would close this.

### LOW-3: LOW-5 judgement (timeouts ignored): partial disagree

`bench_test/cases_release.py:201-204` and `:282`.

- Not judging timeouts is reasonable, since the deliberate trip, OTA and reset
  phases do grow them. Ignoring them entirely, though, hides a link that times
  out continuously all run. A bounded allowance per phase, or a rate check
  outside the deliberate windows, would keep the signal.
- `getattr(..., "crc_errors", 0) or 0` (`:282`) turns an unreadable or None
  counter into 0. An unreadable baseline then reads as a regression, and an
  unreadable final value hides real growth. Record None, and judge
  INCONCLUSIVE when either reading is missing.
- After an RL-D01 install, the ESP reboots. The link counters, heap `min_free`
  and stack high-water marks all restart from boot, so RL-F01 compares
  since-boot watermarks from two different boots. That masks regressions.
  RL-F01 should re-baseline after an install, or report INCONCLUSIVE for those
  fields.

### LOW-4: the LOW-7 pre-check accepts a stage that is busy or has an unverified header

`bench_test/cases_release.py:692`. The pre-check passes when it sees
`staged=False`, whatever `busy=` and `header=` say. A user upload in progress
(`busy=True`), or a written but not yet verified header, is not "staged" yet.
The run then overwrites it and clears it afterwards. `update_stage_release`
refuses an in-flight operation itself (`mcp_server_update.py:406`), but it
does not refuse an unverified header. To refuse anything the run did not
create, the pre-check should also require `busy=False` and `header=none`.

The `"staged=True" in pre` clause is redundant with `"staged=False" not in pre`.
Negtest M05 removed it and was MISSED. That is an equivalent mutant, not a
coverage gap.

### LOW-5: the RL-B01 non-test-trip guard is under-tested

`bench_test/cases_release.py:481`. `test_non_test_trip_is_not_cleared` uses
reason 6, mask 0x0020, where both fields differ from the test trip. Negtest M12
changed `or` to `and` and was MISSED: with that change, a latched trip with
reason 4 and the wrong mask, or mask 0x0008 and the wrong reason, would be
cleared. Add cases where only one of the two fields differs.

Also untested: the path where `safety_test_trip` reports that no trip latched
(pre_reason 0). That path proceeds to clear and then FAILs.

### INFO-1: LOW-3 judgement (LCD only on the bench): agree

Running RL-E01's LCD check only on the bench with a camera is right. A kiln
release run therefore has no LCD coverage. `lcd_checked` is recorded in the
evidence, and `tools/release_gates.py` should keep surfacing it so a kiln-only
release is not read as LCD-verified.

### INFO-2: `allow_heat` defaults to True on both tools

`mcp_server_bench_test.py:35` and `:291`. `confirm=True` alone runs the
release heat phase. This is intended (heat runs are pre-authorized), but it
means the confirm gate is the only gate on heat for `suite="release"`.

### INFO-3: test gaps that the mutations exposed

- No test makes `update_stage_release` raise or return FAILED/UNKNOWN
  (see MED-A; M06 MISSED).
- No identity boundary test or multi-zone test (see MED-B).
- No pre-check test with `busy=True` (see LOW-4).

### INFO-4: M05 is an equivalent mutant

See LOW-4. No test can tell it apart, and none is needed.

## Verified fixed

- MED-4: the release confirm gate is on both MCP entry points, `bench_test_run`
  (`:160`) and `bench_test_start` (`:322`, refused synchronously before any job
  is created). It requires exactly `True` (negtest M10 and M11 CAUGHT).
  `release_gates.py` only reads evidence.
- LOW-4: an unknown target raises `ValueError` (`:92-96`; M09 CAUGHT).
- LOW-6: `actual_valid` defaults to False (`:578`).
- LOW-7: a pre-existing `staged=True` image is refused and left untouched
  (test at `tests/test_bench_test_cases_release.py:428`). LOW-4 above covers
  the remaining hole.
- MED-3, for the paths it covers: a stage result that does not claim read-back
  verification, a failed install, and a stage that raises (by code reading,
  untested) all clear the stage. A failed or raising clear taints the run
  (M07, M13, M14 CAUGHT).
- INFO-1, the RL-A01 abort on FAIL/INCONCLUSIVE, and an unreadable crash
  report recorded as None: verified.

## Negtest

`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_bench_test_cases_release.py
tests/test_bench_test_start_jobs.py tests/test_release_gates.py"`, base
`87721270a`. `-RequireAssertion` is refused in combination with `-Preset`
(`NEGTEST ERROR: -RequireAssertion cannot be combined with -ExpectPattern or a -Preset`),
so the pytest preset's own `^FAILED` pattern was used. Each CAUGHT log was then
checked by hand. Every one fails on an `AssertionError`, or on
`Failed: DID NOT RAISE ValueError` for M09, which is a pytest assertion. No
mutant failed at import or collection. Baseline passed, real tree unchanged,
copies removed. Result: 11 CAUGHT, 3 MISSED.

| # | Mutation | Verdict | Caught by |
|---|----------|---------|-----------|
| M01 | `BENCH_CEILING_MAX_C` 200 -> 2000 | CAUGHT | identity mismatch, kiln ceiling precheck |
| M02 | kiln branch of identity check dropped | CAUGHT | identity mismatch |
| M03 | bench branch of identity check dropped | CAUGHT | identity mismatch |
| M04 | zones read failure fails open | CAUGHT | identity mismatch |
| M05 | `"staged=True" in pre` clause removed | MISSED | equivalent (INFO-4) |
| M06 | stage exception leaves `staged = False` | MISSED | no raising-stage test (MED-A) |
| M07 | failed stage clear does not taint | CAUGHT | failed/raising clear taints |
| M08 | CRC growth tolerance +5 | CAUGHT | compare regressions |
| M09 | unknown target returns `bench` | CAUGHT | unknown target raises |
| M10 | `bench_test_run` release gate truthy | CAUGHT | exact confirm |
| M11 | `bench_test_start` release gate removed | CAUGHT | exact confirm |
| M12 | non-test-trip guard `or` -> `and` | MISSED | single-field test missing (LOW-5) |
| M13 | no-read-back-claim stage accepted | CAUGHT | no-readback-claim fails and clears |
| M14 | install failure skips the clear | CAUGHT | install failed clears stage |

## Fix status

All findings fixed in the PcTools release suite (`bench_test/cases_release.py`, `bench_test/runner.py`, `mcp_server_bench_test.py`). The board was not touched.

- MED-A: RL-D01 re-reads `update_status` after any `update_stage_release` outcome (ok, FAILED, UNKNOWN, raise). A dirty stage (staged, busy or a header) gets `update_fetch_cancel`, then `update_stage_clear`; a failed clear taints the run. A verified stage is always cleared unless an install consumed it. Tests cover raise, FAILED, UNKNOWN, clear failure and a clean re-read.
- MED-B: heat refuses INCONCLUSIVE when `mcp_server_aux._board_identity_check` reports a UART/HTTP mismatch, raises or is unreadable. The 200 C kiln heuristic uses only the zones in the heat mask; a heated zone without a readable ceiling refuses. Tests: 200 vs 200.1 for bench and kiln, multi-zone mask, mismatch writes nothing.
- LOW-1: `BenchTestRunner.run` and `run_suite` refuse suite release unless `confirm is True` (dry run excepted).
- LOW-2: registry test that RL-* ids exist only in suite release.
- LOW-3: an unreadable link counter is INCONCLUSIVE; RL-D01 re-baselines after a successful install (baseline None if the board never returns); link timeouts regress only past `LINK_TIMEOUT_GROWTH_BOUND` (20, chosen not measured), any CRC growth still regresses.
- LOW-4: RL-D01 pre-check requires `staged=False`, `busy=False` and `header=none`.
- LOW-5: RL-B01 tests for reason-only-wrong, mask-only-wrong and no trip latched.
- INFO-2: `allow_heat` defaults to False for suite release in `bench_test_run` and `bench_test_start`; docstrings updated.

Negative tests (`negtest.ps1 -Preset pytest`, each CAUGHT checked as an assertion failure in the log): stage cleanup disabled, pre-check busy/header ignored, timeout bound unbounded, no re-baseline, link-counter check off, runner confirm gate off, ceiling boundary flipped, mask ignored, allow_heat default True in run and in start: all CAUGHT after two added tests (timeout bound, start default) closed initial misses.
