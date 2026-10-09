# Review: board-writing bench judges (2026-10-09)

Scope: `a7be7ba6` (LCD-10/11/12, `tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py`,
`tools/PcTools/tests/test_bench_test_cases_lcd_writers.py`) and `3a593a7c` (OT-E11
`_case_ote11` in `cases_ota.py`, LCD-20 in `cases_lcd.py`). Read-only review against
origin/dev at `a7be7ba6`. Line numbers are origin/dev at that commit.

Bar applied: every write behind `board_lock.write_refusal` or the relevant gate; heat
only with an explicit opt-in; a firing always stopped in `finally` with the executor idle
and every relay (zone relays 1..4 and K4 via Pico `SafetyFlag.RELAY`) verified off;
teardown restores exactly what changed and never touches user slots it did not create or
slot 7; a failed teardown is never a PASS; no forbidden action.

Forbidden actions: none found. Neither commit calls `debug_write_memory`,
`load_config_preset`, `autotune_accept`, `estop_verify`, `factory_reset`, a guard
disable, a Pico flash, a whole-page zones write, `backup_import`, `update_stage_clear`
or `safety_clear_trip`.

## Findings, ranked

### CRITICAL-1: a failed first profile listing makes LCD-10/LCD-11 teardown delete every user profile, slot 7 included

`cases_lcd.py:6082-6087`: `_lcdwr_free_slot()` returns `(None, set())` when
`list_all()` raises. `_lcdwr_save_tmp()` passes that empty set back as `ids_before`
(LCD-10 `:6239`, LCD-11 `:6366` also initialise `ids_before = set()` before the `try`).
The `finally` then calls `_lcdwr_finish(env, result, set())` -> `_lcdwr_delete_new()`,
which computes `extra |= {every id list_all() now returns} - set()` (`:6123`) and deletes
each one. One transient UART timeout on the first `list_all()` followed by a good second
one deletes every user profile on the board, including the hidden bench profile in slot 7
(`BENCH_PROFILE_SLOT_ID`). The UART DELETE handler refuses builtins
(`uart_bridge_ext_control.c:526`), so only builtins survive. The same wipe follows any
exception raised before `_lcdwr_save_tmp()` returns (for example in the
`ProfileSegment(...)` constructor, outside its `try`). The restore check then reports
`restored=False` and FAIL, after the damage. LCD-12 is not affected: it reads
`ids_before` before its `try` and returns INCONCLUSIVE on failure.

Fix: delete only by provenance. Teardown deletes `env["created"]` and nothing else, and
only after a fresh read shows that slot holds this run's own name (`LCD10_TMP`,
`LCD11_TMP`). Use `ids_before = None` for "unknown"; when it is None, delete nothing and
report FAIL "cannot verify". Never delete the diff of two listings. Add a fake test where
the first `list_all()` raises and the second returns user profiles, asserting zero
deletes.

### HIGH-1: LCD-11 keeps tapping stale coordinates and can delete a neighbouring profile

`cases_lcd.py:6396-6405`: all four taps use the `btn` coordinates resolved once before
the first tap, and the sequence continues after `first_kept`/`stale_kept` already show the
transient profile was deleted. That is the defect this case looks for. If the firmware
deletes on one tap, or on the stale tap, the list re-renders, the rows below shift up, and
the remaining taps land on the next row's Delete button. The pattern is arm, then confirm
within 0.4 s, so the neighbouring profile is deleted. That row can be a real user profile
or `BENCH_HP` (slot 7). The restore check (`after == ids_before`) then reports FAIL, but
the profile is gone and cannot be recovered.

Fix: stop tapping as soon as `exists()` is not `True`. Before every tap, re-list the
targets, re-resolve the row by name (`LCD11_TMP`), and require that the Delete or
`Confirm?` button's `cy` matches that row. Abort as INCONCLUSIVE if the row is gone or has
moved. The fake should model rows shifting after a delete.

### HIGH-2: LCD-10 fires with only `allow_heat`, which defaults True; recommend `heat=True` plus a second opt-in

`cases_lcd.py:6214` gates the firing on `ctx["allow_heat"]` only. `runner.py:410/452` and
`mcp_server_bench_test.py:29` default `allow_heat=True`. So a plain
`bench_test_run(suite="lcd")` starts a real firing. Runner comments `runner.py:439-464`
record that this exact gap led LCD-19 (`lcd19_allow_heat`), LCD-22..24
(`lcd22_allow_heat`, `cases_lcd.py:4049/4631`) and OT-E07/E08 (`ota_allow_heat`) to each
gain a second, default-False opt-in. LCD-10 also lacks the registry `heat` flag
(`registry.py:318`). Without it, (a) `allow_heat=False` gives a case-level NOT_RUN instead
of the runner's uniform SKIP (`runner.py:553`), and (b) `registry.py:389` sorts LCD-10
among the read-only LCD cases instead of after them, so a firing starts in the middle of
the read-only LCD captures.

Recommendation: yes, flag it. Set `heat=True` for LCD-10 in `registry.py:318`. Gate the
case on a second default-False opt-in: reuse `lcd_edit_heat`/`lcd22_allow_heat`, or add an
`lcd_start_heat`. Update `test_lcd_suite_and_plan_counts_match`
(`test_bench_test_cases_lcd_edit_refusals.py:1317-1325`) to the five heat ids. Change
`docs/BENCH_TEST_SYSTEM_PLAN.md:339` to `| LCD | 26 | 5 (LCD-10, LCD-22, LCD-23, LCD-24, LCD-25) |`.
The pinned count describes the plan. It does not justify leaving out a case that heats. The
plan's LCD-10 row (`:243`) still says "on the hidden bench slot ... heat via HP-01", but the
code uses a transient user slot. Correct the row in the same change.

### HIGH-3: LCD-10 never verifies the zone relays off

`cases_lcd.py:6311-6330`: the `finally` stops the firing, waits up to 15 s for
idle/done/faulted, reads `_read_energized()` once (K4 through `SAFETY_FLAG_RELAY`,
`dashboard_http.c:333`), and FAILs only on a wrong state or energized. Zone relays 1..4 are
never read. `_lcd22_cleanup()` (`cases_lcd.py:3974-4040`) already polls both K4 and
`/api/profile_exec` `relay_on` per zone (`_lcd22_zone_relays`, `:3945`) inside one bounded
loop. Use that pattern. The single `energized` read after the state loop can also catch K4
mid-release and give a spurious FAIL. That error is in the safe direction, but a bounded
re-poll removes it.

### MEDIUM-1: LCD-10 does not check which profile it started

`cases_lcd.py:6284`: `start_ran` is only `state == "running"`. If the picker or detail page
opened a different row, for example a user profile with a high target or slot 7, the
confirm path fires that profile and the case reports PASS. Require
`exec.profile_id == slot` as soon as the executor reaches running. Otherwise stop at once
and FAIL. The ceiling preflight (`:6232`) covers only the transient target.

### MEDIUM-2: OT-E11 teardown treats an unreadable status as "not in recovery"

`cases_ota.py:2596`: when `status_fn()` raises after `recovery_exit`, `still = False`. The
case records no taint and reports no stuck board, so the unknown state passes as success.
`recovery_exit` already returns `ok - ...` only after the application answers
(`mcp_server_recovery.py:427`). Taint the run unless `_tool_ok(exit_text)`, and record an
unreadable status as `None`, not False.

### MEDIUM-3: OT-E11 skips teardown when `recovery_enter` reports an error after the board may have rebooted

`cases_ota.py:2548`: any text other than `ok` sets `entered=False` and returns INCONCLUSIVE
"board unchanged", and the `finally` skips `recovery_exit`. `recovery_enter` returns
`error: {exc}` for a transport failure (`mcp_server_ota.py:254`). A reset or timeout after
the board accepted the POST and started rebooting would leave it in recovery while the
report says it is unchanged. On an `error:` (not `error: refused`) reply, probe
`recovery_status` before claiming the board is unchanged, and run the exit path if it
answers as recovery.

### MEDIUM-4: OT-E11 has no `write_refusal` gate and never reads safety state afterward

`cases_ota.py:2510`: the case reboots the board into recovery and rewrites the app
partition. It is gated by idle, image path and interlock, but not by
`board_lock.write_refusal(ctx)`. No other OT case uses `write_refusal` either (they rely on
`ota_matrix_run`'s `confirm` and its run-level gate), but a direct
`bench_test_run(cases=["OT-E11"], ota_image_path=...)` reaches it without the fail-closed
suite check. Add the gate. Separately, `_safety_status_fn` (`:2579`) defaults to None, so a
real run never reads the Pico after the recovery dwell. That dwell latches S6b. This judge
must not clear the trip (only S6a, mask 0x0020, may be cleared), so it should read
`safety_get_status` and report or taint a latched trip, not leave it for the next case's
preflight to discover.

### LOW

- `cases_lcd.py:6114-6130`: even without CRITICAL-1, `_lcdwr_delete_new` deletes every id
  that appeared during the run, which is inference, not provenance. This affects all three
  cases, including LCD-12, where the Save cell is chosen through the UI. Delete
  `env["created"]` only, after a name check.
- `cases_lcd.py:6505`: LCD-12 skips the bench slot with a literal `7`.
  `cases_heat.py:40-43` says every case must use `BENCH_PROFILE_SLOT_ID`, which moves to 101
  under slots100. Use the constant.
- `cases_lcd.py:6061`: `_LCDWR_SLOT_RANGE = range(0, 16)` while `PROFILES_MAX_COUNT` is 100
  (`profiles_types.h:30`). This is harmless but arbitrary. Derive it, or name the reason.
- `cases_lcd.py:6311`: stop is called only if `started` or the state is `running`. A
  pre-confirm path that leaves the executor in any other non-terminal state (paused) is not
  stopped. Call stop whenever the state is not idle/done/faulted.
- `cases_lcd.py:6231`: target = min(all zone temps) + 15 with no absolute cap, unlike
  LCD-22 (`_LCD22_MAX_TARGET_C = 60`) and the plan's 70 C rule. The firing lasts seconds,
  so the risk is small. Add the cap for consistency.
- `cases_lcd.py:6167-6188`: an unrestored profile list downgrades only a PASS. It does not
  taint the run (`ctx["_taint"]`), so an INCONCLUSIVE over a dirty board is silent at run
  level. If the case raises, nothing reports the cleanup result.
- Tests: `test_bench_test_cases_lcd_writers.py` covers the gates and judges and runs
  LCD-11/12 against fakes. Nothing runs LCD-10's start/stop/finally path against a fake
  executor, so the stop, relay and FAIL-on-dirty logic has no test.
- `cases_ota.py:2481` (`_tool_ok`, used at `:2571`): `ok-with-warning - ... boot_guard was NOT ...` from
  `recovery_push_esp_image` counts as a clean PASS, and the warning is only in
  `observed`. Put it in the reason.

## LCD-20

`observe_recovery_idle` (`cases_lcd.py:6022-6051`) only waits, captures and samples the
panel. It makes no board writes. INCONCLUSIVE on a missing capture or sample is correct. No
findings.

## Verdict

Do not run LCD-10 or LCD-11 on the bench until CRITICAL-1 and HIGH-1 are fixed: each can
destroy user profiles it did not create. LCD-10 also needs HIGH-2 and HIGH-3 before it
counts as heat-safe. LCD-12 is safe to run, with the LOW provenance caveat. OT-E11 is
usable, but fix MEDIUM-2 and MEDIUM-3 so a board stranded in recovery is always reported.
