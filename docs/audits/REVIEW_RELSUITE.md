# Review RELSUITE: release bench suite (af1097175)

Date: 2026-10-13. Reviewer: Opus, adversarial. Base: origin/dev `af1097175`
(pushed directly, without `land.ps1`).

Scope:
- `tools/PcTools/src/kilnctrl/bench_test/cases_release.py` (RL-A01, RL-B01,
  RL-C01, RL-D01, RL-E01, RL-F01, RL-V01)
- the `target` and `ota_install` parameters on `bench_test_run` and
  `bench_test_start` (`mcp_server_bench_test.py`)
- the `tools/release_gates.py` `--suites` default
- `tools/PcTools/tests/test_bench_test_cases_release.py`

Not run on hardware. pytest in a clean worktree:
`test_bench_test_cases_release.py` and `test_bench_test_start_jobs.py`,
60 passed; with `test_release_gates.py` added, 95 passed.

Totals: 0 HIGH, 6 MED, 7 LOW, 1 INFO.

## Verified clean

- **E-stop rule.** No release case calls `estop_verify`; the name appears only
  in docstrings.
- **Heat gate.** RL-C01 SKIPs unless `ctx["allow_heat"] is True` exactly, and
  touches nothing before that check.
- **Teardown.** RL-C01 runs `_release_teardown` in a `finally`.
  `_release_teardown` calls `H._cleanup_bench_profile` (stop and delete, never
  raises), then re-reads the executor. If the executor is still
  running/paused, or a relay is still commanded ON, it taints and aborts. A
  failed re-read also taints.
- **Phase-A FAIL aborts.** A FAIL in RL-A01 calls `_abort`, and every later
  case returns NOT_RUN.
- **Heap floor.** `min_free < 8192` is a FAIL and 8192 is a PASS (owner floor:
  at least 8192 B). Unreadable or bool values are INCONCLUSIVE.
- **RL-V01.** A missing phase is a FAIL. Any FAIL is a FAIL. All PASS is a
  PASS. Anything else is INCONCLUSIVE, so SKIP and NOT_RUN never count toward
  a PASS.
- **release_gates.** `EXPECTED_INCONCLUSIVE` has no `release` entry, so a
  release run counts only when every case is PASS. A SKIP or INCONCLUSIVE
  disqualifies it.
- **Target isolation.** `target="kiln"` and `target="bench"` select disjoint
  profile paths in RL-C01. `bench_test_run` validates `target` against
  `TARGETS`.
- **OTA install tool absent.** With `ota_install=True` and no
  `update_install_staged` tool, RL-D01 SKIPs honestly. It does not PASS. See
  MED-3 for the stage left behind.
- **OTA confirm on start.** `bench_test_start` refuses `ota_install=True`
  without `confirm=True`, synchronously.

## Findings

### MED-1: crash-report check error reads as "no crash" (false green)

`_snapshot` (cases_release.py:253) sets
`crash_unacked = bool(h.get("unacknowledged_crash"))`.

`get_heap_status` reports a failed `GET /api/crash_report` read through
`unacknowledged_crash_check_error`, and in that case leaves
`unacknowledged_crash` falsy. The suite ignores that field. So an unreadable
crash report becomes `crash_unacked=False`, and RL-A01 or RL-F01 can PASS on a
board that panicked.

Fix: when `unacknowledged_crash_check_error` is set, record
`crash_unacked=None` and judge it INCONCLUSIVE.

Negtest: the mutant that replaces the check with `False` is MISSED. No test
covers a crash-report check error.

### MED-2: RL-D01 stage-clear failure is never detected (false green)

cases_release.py:647-652, the `finally` block:

```python
if "FAIL" in str(clr).upper().split(" ")[0:2] or str(clr).startswith(("REFUSED", "error")):
    ctx["_tainted"] = True
return CaseResult(Verdict.PASS, observed=obs)
```

Two problems:

1. `update_stage_clear` reports failure as `"FAILED: ..."` or
   `"UNKNOWN: ..."`. The token check compares the first word, `"FAILED:"`,
   against `"FAIL"` with list membership, so it never matches. `UNKNOWN` is
   not checked at all.
2. Even a `REFUSED` or `error` result only taints the run. The case still
   returns PASS.

So a staged image can stay on the board while RL-D01 reports PASS.

Fix: treat anything not starting with `"ok"` as a failed clear. Return FAIL
(or INCONCLUSIVE for UNKNOWN) and taint.

Negtest: the mutant that removes the taint line is MISSED.

### MED-3: `ota_install=True` with the install tool absent leaves the stage on the board

When `ota_install is True`, the `finally` block skips the clear. That is right
when the install runs. When `update_install_staged` is absent, though, the case
SKIPs and returns early, and the freshly staged image stays in the `stage`
partition with nothing to install it.

Fix: check that the install tool exists before staging, or clear the stage
whenever the install did not run.

### MED-4: board writes happen without a user confirm

`bench_test_run(suite="release")` requires `confirm=True` only when an
`ota_*`/`update_*` image argument is set or `ota_install=True`. A plain
`bench_test_run(suite="release")` call with no confirm still runs these writes,
because the suite passes `confirm=True` internally:

- RL-B01 `safety_test_trip(confirm=True)`, which latches a real Pico trip and
  then clears it.
- RL-D01 `update_stage_release(confirm=True)`, which downloads into the stage
  partition.
- RL-D01 `update_stage_clear(confirm=True)`, which erases the stage partition.

Other suites that write the board gate on a run-level confirm. Bench writes are
pre-authorized by the owner, but the tool's own contract says writes are
confirm-gated.

Fix: require `confirm is True` for suite `release`, in both `bench_test_run`
and `bench_test_start`.

### MED-5: kiln heat path bypasses the zone-ceiling check and has no bench/kiln detection

On `target="kiln"`, RL-C01 calls `_start_bench_profile(..., target_c=100)`
directly. That bypasses the `_check_zone_ceilings` precheck that the other heat
cases go through.

Nothing checks that the board really is the target named. A real kiln run with
`target="bench"` gets the bench profile and timeout. A bench fixture run with
`target="kiln"` waits for a 100 C hold that a 4 W fixture cannot reach, and so
heats until the kiln timeout.

Fix: run the ceiling precheck on both paths. Cross-check `target` against a
board fact (for example `kiln_configs`, or the relay/thermocouple topology)
and refuse a mismatch.

### MED-6: RL-A01 INCONCLUSIVE does not stop the heat phase

RL-A01 aborts only on FAIL (cases_release.py:421). An INCONCLUSIVE baseline (an
unreadable heap, readiness, link or crash report) lets RL-B01's real trip and
RL-C01's unattended firing go ahead on a board whose health is unknown. RL-V01
will mark the run INCONCLUSIVE, but only after the heat has run.

Fix: abort before RL-B01/RL-C01 when RL-A01 is not PASS, or at least before
RL-C01.

Test: `test_phase_a_inconclusive_does_not_abort` pins the current behavior.

### LOW-1: RL-B01 clears any latched trip, and a raising trip tool does not abort

`trip_fn(confirm=True)` runs outside the `try`. If it raises, the runner turns
that into a FAIL, but `_abort` is never called, so RL-C01 still heats after a
failed safety phase.

Separately, the clear step clears whatever trip is latched. If a real (non-test)
trip latched alongside the test trip, it is cleared without the reason ever
being checked against 4.

### LOW-2: `_install_and_verify` is weak

- The `before` snapshot is never compared with the result, so the check does
  not prove the version changed.
- A snapshot that answers but has no `fw_build` is still accepted.

### LOW-3: RL-E01 can never PASS on target=kiln

RL-E01 calls `spec.judge` directly, which bypasses the runner's preconditions.
On kiln it is always INCONCLUSIVE, so a `target=kiln` release run can never
reach a release-gate PASS.

### LOW-4: `_target` fails open to "bench"

`_target` maps an unknown value to `"bench"`. The MCP tool validates `target`
first, but a direct runner caller with a typo gets the bench profile silently.

Negtest: the mutant that forces `"bench"` is MISSED. No test runs the kiln
path through `_target`.

### LOW-5: link `timeouts` growth may report a false regression in RL-F01

`compare_final_to_baseline` treats any increase in link error counters as a
regression. RL-B01's trip/clear and normal link traffic can bump `timeouts`, so
RL-F01 may FAIL on a healthy board. Bench evidence is needed to settle it.

### LOW-6: hold check defaults `actual_valid` to True

In the RL-C01 hold loop, a zone object with no `actual_valid` attribute is
treated as valid.

### LOW-7: an image the user staged before the run is overwritten and erased

RL-D01 stages over the `stage` partition and then clears it, discarding any
image the user had staged before the run. Nothing records or restores it.

### INFO-1: bench evidence does not record target or install

`release_gates bench-evidence` checks the suites and `fw_build`, but does not
check that the release run used the intended `target` or ran the install. Also,
after an install the board's `fw_build` changes partway through the run, so
evidence keyed on the starting build needs care.

Negtest: the mutant that drops `release` from the `--suites` default is
MISSED. No test exercises the default.

## Negative-test table

Command, from the worktree at `af1097175`:

`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_bench_test_cases_release.py tests/test_bench_test_start_jobs.py tests/test_release_gates.py"`

Baseline passed (95 passed). `real_tree_unchanged: true`. Each log was checked:
every CAUGHT failed on an `AssertionError`, with no collection or import error.

| # | Mutation | Verdict | Failing test |
|---|----------|---------|--------------|
| 1 | heap `min_free < floor` to `<= floor + 1` | CAUGHT | test_heap_floor_boundary |
| 2 | heat gate `is not True` to `is None` | CAUGHT | test_heat_without_allow_heat_skips_and_touches_nothing |
| 3 | RL-A01 `_abort` removed | CAUGHT | test_phase_a_fail_aborts_and_later_cases_not_run[bad0..5] |
| 4 | RL-V01 `vals == {"PASS"}` to `"PASS" in vals` | CAUGHT | test_verdict_line_one_per_phase |
| 5 | RL-C01 `_release_teardown` removed from `finally` | CAUGHT | 4 heat teardown tests |
| 6 | RL-D01 `update_stage_clear` call removed | CAUGHT | test_ota_pass_clears_stage_by_default, test_ota_stage_without_readback_claim_fails_and_still_clears |
| 7 | RL-D01 clear-failure taint removed | MISSED | none (MED-2) |
| 8 | `_target` always "bench" | MISSED | none (LOW-4) |
| 9 | crash check replaced with `False` | MISSED | none (MED-1) |
| 10 | `bench_test_start` `or ota_install is True` removed | CAUGHT | test_release_ota_install_is_ota_gated |
| 11 | release_gates `--suites` default drops `release` | MISSED | none (INFO-1) |
