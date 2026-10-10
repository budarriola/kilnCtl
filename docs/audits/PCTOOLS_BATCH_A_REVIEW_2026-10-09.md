# PcTools write-tools batch A review (2026-10-09)

Reviews commit `bb92ec5d4` on origin/dev ("PcTools write-tool fixes, tooling batch A").
The yardstick is `docs/audits/PCTOOLS_WRITE_TOOLS_REVIEW_2026-10-09.md`, whose status
column was added in `a691786c0`. This is a review only; nothing here is fixed.
Paths are under `tools/PcTools/`. Line numbers are at `a691786c0`.

Method:
- Read the diff.
- Cross-checked the zone strip regex against
  `firmware/KilnFW/App/drivers/http/zones_http_post_parse.c`, and the trip-mask logic against
  `firmware/SaftyFW/src/tasks/link_task.c` / `link_frame.c`.
- Ran the full PcTools pytest suite at `a691786c0`: **3 failed, 6731 passed**.

## Summary

| # | Sev | Item |
|---|-----|------|
| 1 | MED | Two zone writer HappyPath tests now fail (strip rewrites mock body) |
| 2 | MED | capability_preflight ramp_assist tripwire broken by the apply_preset refactor |
| 3 | MED | `preset_partial` says "nothing had landed" after earlier zones' PID landed |
| 4 | LOW | `safety_clear_trip` precheck is skipped when DIAG was never received |
| 5 | LOW | Mask check cannot fire on real firmware; read-back races the Pico |
| 6 | LOW | `apply_zone_preset` keeps omit-preserved keys a preset names in GET spelling |
| 7 | LOW | No test proves `control_set_zone_limits`/`_type` actually strip |
| 8 | LOW | `unittest.main()` placed before the last test class |
| 9 | LOW | `ApplyPresetPartialTests` is vacuous for the real partial path |
| 10 | LOW | Several "fixed" items have no test at all |
| 11 | LOW | `ui_run_script` lets non-ConfigPresetError preset failures escape raw |
| 12 | INFO | `ui_run_script` default flip skips script-named presets silently |
| 13 | INFO | Items verified correct |

## Findings

### 1. MED: zone writer HappyPath tests fail on origin/dev

**Where:** `src/kilnctrl/mcp_server_control.py:807` (set_zone_limits) and `:990` (set_zone_type).
Both now pass the POST body through `_strip_omit_preserved_zone_fields(..., None)` (`:1135`),
which round-trips the body through `parse_qsl`/`urlencode`.

**What fails:** the existing mocks build the body from a placeholder string `'body'`. The
round trip turns it into `'body='`, so the asserted POST body no longer matches. Failing tests:
- `tests/test_mcp_server_control_zone_limits.py::HappyPathTest::test_applies_and_confirms_by_readback`
- `tests/test_mcp_server_control_zone_type.py::HappyPathTest::test_applies_and_confirms_by_readback`

**Scenario:** any lander running the affected pytest files sees red on dev. The coordinator's
full run on the dev tip reports these as NEW failures against main. This also shows the
commit was not checked with the full pytest suite.

### 2. MED: capability_preflight manifest tripwire broken

**Where:** `tests/test_capability_preflight.py::ManifestMatchesApplyPresetTest::test_manifest_matches_apply_preset_ramp_assist_pin`.
It raises ValueError at line 359.

**Cause:** the tripwire calls `inspect.getsource(config_presets.apply_preset)` and searches it
for `if zones_host:`. The refactor moved that body into `_apply_preset_stages`
(`src/kilnctrl/config_presets.py:464`, with `if zones_host:` at about line 497).
`apply_preset` is now only the try/except wrapper.

**Scenario:** the third failure of the 3. Until the tripwire is pointed at
`_apply_preset_stages`, it guards nothing. A later change that drops the ramp_assist pin from
the preset path would not be caught by this test.

### 3. MED: partial-write report is wrong for a mid-loop UART failure

**Where:** `src/kilnctrl/config_presets.py:466-494` and `:454`. `done.append("PID/model written
over UART: ...")` runs only after the whole per-zone loop and the PID read-back have finished.

**Scenario:**
1. A 3-zone preset is applied.
2. `set_zone_pid` succeeds for zones 0 and 1.
3. For zone 2, `set_zone_pid` or `set_zone_model` raises (for example a `ControlQueryError`
   UART timeout).
4. `done` is still empty, so `apply_preset` attaches
   `preset_partial = "(nothing had landed yet)"`.
5. `load_config_preset`, `ui_run_script` and `factory_default_then_load_preset` relay that
   text. The operator is told nothing was written, yet two zones now carry the preset's gains.

The PID read-back (`_verify_pid_readback`) is also skipped on this path, so nobody verifies the
zones that did land.

**Status:** the review doc marks partial-write reporting as fixed. That is true only for
failures between stages, not for failures inside the UART stage.

**Fix direction (not applied):** append per zone inside the loop.

### 4. LOW: `safety_clear_trip` precheck fails open when DIAG is unknown

**Where:** `src/kilnctrl/mcp_server_safety.py:336`. The whole mask/reason precheck sits under
`if before.ever_received:`.

**Scenario:** the link has never delivered a DIAG frame. This happens just after a dual
reflash, which is exactly when CLAUDE.md says to check `trip_reason`/`trip_mask` first. In
this state the tool sends the clear with no check at all. The docstring says the tool
"refuses when the link is down", but it does not.

The tool should refuse in this state (or require an explicit override), because it cannot show
what it is about to clear.

### 5. LOW: mask check cannot trigger on real firmware; read-back races the clear

**Where:** `src/kilnctrl/mcp_server_safety.py:337-341` (check) and about `:349` (read-back).

**Why the check cannot fire:** the Pico's DIAG `trip_mask` is always derived from
`trip_reason`, as `1 << (reason-1)` (`firmware/SaftyFW/src/tasks/link_task.c:1066`,
`link_frame.c:248-254`). `trip_mask != expected` therefore cannot happen on real hardware.
The "more than one guard latched" case the docstring describes cannot occur, and
`allow_unexpected_mask` (correctly strict, `is not True`) is effectively a dead override.
The check is harmless as defence in depth, but it adds no coverage.

**Read-back race:** the read-back runs immediately after a fire-and-forget clear, against the
cached DIAG (Frame B push).

**Scenario:** the Pico processes the clear a few hundred ms later. The tool has already
reported "STILL LATCHED" for a clear that succeeded, and the operator may clear again or
escalate needlessly.

**Fix direction:** poll the read-back for a bounded window, or wait for the next DIAG frame.

### 6. LOW: `apply_zone_preset` re-posts omit-preserved fields at GET rounding for GET-spelled presets

**Where:** `src/kilnctrl/zones_http_client.py:1277` (`_preset_named_omit_preserved_keys`), used
by `apply_zone_preset` (about `:1328`).

**What it does:**
- It keeps `z{i}_k`/`z{i}_tau`/`z{i}_deadtime` whenever the preset carries
  `model_k_dc`/`model_tau_s`/`model_dead_time_s`. Those are the GET/backup spellings, mapped in
  `_ZONE_FIELD_FORM_KEY`.
- It keeps `z{i}_coupling_cN` whenever the preset copies the individual GET `coupling_cN` keys.

**Why that is wrong:** `build_post_body` does not post those preset values (they are
KNOWN_IGNORED/reference-only); it echoes GET's rounded print. The kept keys are therefore
re-posted at GET rounding (tau/deadtime at `%.1f`).

**Scenario:** someone authors a preset by copying `GET /api/zones` output, which is the most
natural path. Every apply silently re-rounds the plant model and coupling cells, which is the
drift the strip was added to prevent.

**Fix direction:** keep a key only if `build_post_body` actually takes its value from the preset.

### 7. LOW: no test proves the two zone writers strip

**Where:** `tests/test_pctools_write_review_2026_10_09.py:47` (`ZoneStripTests`). It tests only
`ZONE_OMIT_PRESERVED_KEY_RE` and the helper in isolation.

**Scenario:** reverting the wrapping at `mcp_server_control.py:807`/`:990` would pass every
test. Today's only signal is the two HappyPath tests in finding 1, and they fail in the opposite
direction.

A test should assert that the posted body for `control_set_zone_limits`/`control_set_zone_type`
contains no `coupling_c`, `_k`, `_tau`, `_deadtime` or `fuzzy_strength` keys.
`apply_zone_preset` stripping is covered by the edited `test_zones_http_client` tests.

### 8. LOW: `unittest.main()` hides the last test class

**Where:** `tests/test_pctools_write_review_2026_10_09.py:108`. The
`if __name__ == "__main__": unittest.main()` block comes before
`class SafetyClearTripPrecheckTests` (`:112`).

**Scenario:** someone runs `python tests/test_pctools_write_review_2026_10_09.py` directly.
`unittest.main()` runs and exits before the class is defined, so the 3 safety tests silently
never run. pytest is unaffected.

### 9. LOW: `ApplyPresetPartialTests` does not test the partial content

**Where:** `tests/test_pctools_write_review_2026_10_09.py:75`. The test patches
`_apply_preset_stages` wholesale and asserts only that `preset_partial` is attached.

**Scenario:** it cannot catch finding 3, because it never runs the real stage code. A test that
makes `set_zone_pid` raise on the second zone would have caught it.

### 10. LOW: items marked fixed with no test

The following fixes have no test. Each would survive a revert:
- `factory_default_then_load_preset`:
  - the precheck runs before backup and erase;
  - the BOARD WIPED message is shown when `apply_preset` returns `all_ok=False` (not only on an
    exception).
- `control_convert_onoff_zone_to_aux`: the read-back after a 5xx or a timeout
  (`mcp_server_aux.py`, `_conversion_state_after_failure`).
- `thermo_write_reg`: the read-back mismatch path (`mcp_server_thermo.py:271`).
- `aux_http_client.get_stored_relay_io_hits`: failing closed.
- `profile_save_bench_aux_rule`: 5xx reported as an error rather than a refusal.
- `load_config_preset`: the "FAILED (partial apply)" line.

The other test edits in the commit only patch the new run gate or readiness, so existing tests
keep passing.

### 11. LOW: `ui_run_script` lets preset failures escape raw

**Where:** `src/kilnctrl/mcp_server_ui_test.py:121`. Only
`UiScriptError`/`ConfigPresetError` are caught.

**Scenario:** `apply_preset=True` and `apply_preset` raises `ControlQueryError`,
`ZonesHttpError`, `SafetyCfgHttpError` or `RampAssistHttpError` partway through. The exception
reaches the facade as a raw traceback, and the attached `preset_partial` (the reason that
attribute exists) is never shown.

`factory_default_then_load_preset` (`:279-286`) handles the same exception set correctly.

### 12. INFO: default flip skips script presets silently

**Where:** `src/kilnctrl/mcp_server_ui_test.py:84`. `apply_preset` now defaults to False.

Both `ui_scripts/home_start_stop_zone0_lcd.json` and `_web.json` name preset
`ui_test_baseline`. With the default flip they now run against whatever config the board holds,
and the result JSON does not say the preset was skipped. No caller of
`ui_run_script`/`run_ui_script` was found (no bench_test case), so nothing breaks today, and
`test_ui_test_runner` passes. The result should say "preset X skipped (apply_preset=False)".

### 13. INFO: verified correct

- **Zone strip regex** (`zones_http_client.py:1263`) matches the firmware's omit-preserved set
  exactly:
  - hystc (`zones_http_post_parse.c:258-268`)
  - coilpower (`:316-329`)
  - k/tau/deadtime (`:772-805`)
  - fuzzy_strength, coupling_diag_k_dc, easeoffmult, approachratecap, errorband, rateband,
    progressband
  - `coupling_c%u` (`:975-985`)

  No required field is stripped. minons/minoffs are omit-preserved but not stripped; that is
  harmless because they are integers and print losslessly.
- **thermo gates** (`mcp_server_thermo.py:106-121`): all five writers (config_channel,
  set_thresholds, set_cj_offset, clear_faults, write_reg) require strict `confirm is True` and
  the fail-closed run gate. The write_reg read-back compares ints correctly
  (bytes iterate as ints). A read-back failure is reported as "(read-back failed)", not as
  verified. thermo_one_shot and set_auto_report stay ungated, which is reasonable.
- **safety_set_fault_out:** de-asserting requires confirm; asserting (the safe direction) stays
  ungated.
- **safety_clear_trip:** `allow_unexpected_mask` is strict-bool. The expected mask uses
  `1 << (reason-1)`.
- **factory_default_then_load_preset:** the preset precheck runs before the backup and the
  erase. After the erase, every failure path (the typed excepts, the generic except and
  `all_ok=False`) names the backup via `_wiped_message` (`:153`).
- **convert aux:**
  - After a POST failure, the tool reads back unless `_gate_or_error` labels the status
    "refused:" (400/403/409/503). A 5xx or timeout therefore gets a read-back.
  - The post-success readiness check is correct, because `zone_aux_conversion` is listed only
    while the journal marker exists (`readiness_http.c:1107-1125`).
  - The collateral keys were extended.
- **Deferred rows** (backup_import, the generation token, skip_backup strict-bool, the misc
  LOW batch) are correctly marked not done.
- **Not examined:** whether `_verify_pid_readback` compares against inherited
  (`settings_source`) gains, which could give a false mismatch. Speculative; not verified.
