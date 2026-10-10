# Review: kilnctrl MCP tool gates (2026-10-10)

Fresh opus audit of the PcTools kilnctrl MCP surface
(`tools/PcTools/src/kilnctrl/mcp_server*.py` and the clients they call) on
origin/dev (worktree base `8eba89bd2`; the estop section was read from
origin/dev `378d76eab`/`24a5f6116`). No code was changed. Checked per mutating
tool: exact `confirm is True` gate where documented, read-back verification
where documented and whether a failed read-back is reported as a failure,
mid-run refusals, credential echo, timeouts against the client's 300 s abort,
gate pass-through in background jobs, read-only tools issuing only GETs, and
the facade registry count.

## Summary

| Sev | Count | Headline |
|-----|-------|----------|
| HIGH | 3 | aux manual read-back ignores relay-unknown; Pico GPIO tools leave the core halted; Pico GPIO tools have no ARMED gate |
| MED | 11 | several "ok" results on failed/unavailable read-backs; capability_preflight fails open; update_check listed read-only but POSTs; two waits default >= 300 s |
| LOW | ~20 | uncapped waits, ungated diagnostic writers, minor read-back gaps |
| INFO | - | registry count 231 matches CLAUDE.md; every MCP-level confirm gate is exact; background twins pass every gate; estop heat-only gate holds |

The running kilnctrl server reports 229 tools (stale); importing
`kilnctrl.mcp_server` from this tree gives `len(registry.entries) == 231`,
matching CLAUDE.md and `docs/MCP_SERVERS.md`.

## HIGH

### H1. `control_set_aux_manual` verifies against a relay shadow it does not validate

`mcp_server_aux.py:226`: the read-back is `if state.relay(relay) != on:`. It
never consults `IoState.relay_state_unknown` (FLAG_RELAY_UNKNOWN 0x04) or
`i2c_failed`, which `devices_io.py:356-370` says must be treated "as a fault,
not as OFF". An `on=False` request while the expander state is unknown reads
the shadow as OFF and returns "ok ... confirmed by relay shadow read-back".
The read-back also goes over UART (`_srv._io`) while the write goes over HTTP
to the resolved host; nothing checks both reach the same board.

Missing tests: unknown flag set with `on=False` must FAIL; `i2c_failed` must
FAIL. The existing `_io()` mock is a bare `Mock`, so the flags must be set
False explicitly in the happy-path mock for a fix to stay green.

### H2. `pico_gpio_set_mode` / `pico_gpio_write` leave the RP2040 core halted

`debug_probe.write_memory` (`debug_probe.py:857`) issues
`init; halt; <cmd>; exit` with no `resume`. `pico_gpio_probe.set_mode` ends on
`_write_word` (`pico_gpio_probe.py:171-184`), so it leaves the core halted.
`pico_gpio_write` resumes only as a side effect of its read-back read; if that
read raises, the core stays halted. A halted safety processor stops its
guards and drops the link (S6b on the ESP side). This contradicts the owner
rule "halt via probe, always resume". `debug_write_memory` shares the same
no-resume write path.

Missing test: a fake OpenOCD command recorder asserting every write sequence
ends with `resume` (or a following read that resumes), including when the
read-back raises. Existing tests mock `pico_gpio_probe` out entirely.

### H3. `pico_gpio_*` writers have no ARMED gate

`debug_write_memory` (`mcp_server_debug.py` near 726) calls
`debug_probe.pico_armed_state()` and fails closed. The `pico_gpio_set_mode` /
`pico_gpio_write` tools never call it, although their docstring
(`mcp_server_pico_gpio_probe.py:92`) says "same as debug_write_memory". Only
GPIO6 is denied. The module docstring (`pico_gpio_probe.py:36-41`) claiming
relay state cannot be read is stale.

Missing test: ARMED (and unreadable ARMED) refuses with no probe I/O.

## MED

### M1. Wi-Fi writers report "ok" when the read-back is unavailable

- `mcp_server_wifi.py:225` `wifi_add_network`: `note = "" if present else " (read-back unavailable; unverified)"` then `return f"ok - saved ..."`.
- `mcp_server_wifi.py:251` `wifi_set_mode`: `return f"ok - mode set to {mode} (read-back unavailable; unverified)"`.

`wifi_forget` handles the same case correctly. Negtests N5/N6 (turn these
branches into FAILED) were MISSED: no test pins them.

### M2. `thermo_write_reg` reports "ok" on a read-back exception

`mcp_server_thermo.py:288`:
`return f"ok - channel {channel} reg 0x{reg:02X} written (read-back failed: {exc})"`.
A value mismatch returns only "warning -". No test covers the exception path.

### M3. `capability_preflight` fails open on unreadable crash report / readiness / liveness

`capability_preflight.py:454-463`: `except PreflightTransportError: crash = None`,
so a 401 (crash_report is ROUTE_TIER_ADMIN), 5xx or timeout reads as "no
crash"; `crash.get("acknowledged", True)` treats a missing field as
acknowledged. `/api/readiness` fails open the same way. Task liveness
(`mcp_server_capability_preflight.py:38-72`) silently skips when the UART read
or the `.ps1` parse fails. `zone_current_sweep_start` relies on this
preflight before energizing relays; the bench runner relies on it too (and
does fail closed on a report that could not be produced at all).

Negtest N3 (flip to fail-closed) was CAUGHT by
`test_capability_preflight.py::AllPresentTest::test_all_present_is_ok` and
`::BenignMissingTest::test_benign_missing_capability`: the existing fakes
exercise the fail-open path as the normal case, so a fix must update them.
Missing test: crash_report 401 must not read as "no crash".

### M4. `profile_live_*` results start with "ok -" even when the read-back failed

`mcp_server_profile_live.py:109/164/212`:
`return f"ok - {obj} (host={resolved}){_live_readback(...)}"`, so a
"FAILED read-back" or "UNVERIFIED" suffix still follows "ok -". The edit
read-back (near line 50) compares only `name`. Tests use `assertIn` and never
assert the result does not start with "ok". N4 (read-back always OK) was
CAUGHT, but the prefix problem is unpinned.

### M5. Adaptive-tune client decodes tolerantly, so a missing key verifies a write

`adaptive_tune_http_client.py:144/163`: `enabled=bool(entry.get("enabled", False))`
and `revert_available=...get(..., False)`, so an absent key "verifies" a
disable or a revert. `enabled` is not type-checked. N10 was CAUGHT by
`test_missing_fields_default_rather_than_raise`, which deliberately pins the
tolerant default; a fix needs that test changed.

### M6. `update_check` is documented read-only but POSTs

`mcp_server_update.py` module docstring (line 6) lists it under read-only, but
it calls `uhc.start_check`, i.e. `_empty_post(... "/api/update/check")`, which
starts a TLS job on the board. No confirm gate. Either reclassify it in the
docs and CLAUDE.md, or gate it.

### M7. Waits that default at or above the client's 300 s abort, with no background twin

- `recovery_pico_upload` defaults `wait_s=600.0` (`mcp_server_recovery.py:541`).
- `update_stage_release` defaults `wait_s=300.0` plus its prechecks.

No clamp. The client gives up while the board keeps working, and the caller
never sees the verified result. Clamp below 300 s or add a `*_start` twin on
`mcpkit/build_jobs.py`.

### M8. Safety-config writers' mid-run gate is a deny-list

`mcp_server_safety.py:1208/1216`: `if prof.state in (1, 2)` and
`at.state in (1, 2, 3, 4)`, so an unknown or newly added state counts as idle.
`control.py:544` uses an allow-list instead. Same pattern in
`mcp_server_coordinated_gpio_test.py:66`. Affects `safety_set_tc_type`,
`safety_set_commissioning_fields`, `safety_set_rate_guard`.

### M9. `backup_import` and `wifi_set_ap_identity`

`backup_import` (`mcp_server_info.py:1199`) reports `ok - restored` on any
2xx with no data read-back; a failed readiness re-read becomes a string inside
an "ok" result. `wifi_set_ap_identity` has no confirm gate, no mid-run refusal
and no read-back.

### M10. Secrets passed as tool arguments

`wifi_add_network(password=...)` and `wifi_set_ap_identity(ap_password=...)`
put the secret in the client transcript (the call log records keys only, so
the server side is clean). TOTP and web-auth already read secrets from the
environment; do the same here.

### M11. `profile_save_bench_aux_rule` "every OTHER profile unchanged" is shallow

It compares only `name`, `zone_mask` and `segment_count`; segment contents and
rules of other profiles could change unnoticed.

## LOW

- `recovery_push_esp_image` worst case is about 280-290 s, close to the abort.
- `ota_update_*` catch only `OtaHttpError`; `HttpAuthError` and `TimeoutError` escape unstructured.
- Several truthy `body.get("ok")` checks where a string "false" would pass.
- Uncapped waits: `debug_reset` `verify_window_s`, recovery `wait_s`, `safety_capture_ct_counts` `seconds`, `saleae_capture` duration (which also accepts any output path).
- network `verify_timeout_s` is capped at 600, above 300.
- `cfgfs_format` reports WARNING rather than FAILED when the confirm read-back is unreadable.
- `web_auth_setup_http_client.py:199` `try_login` accepts any 200 without checking for the session cookie.
- `http_auth.urlopen(timeout=None)` is a latent hang.
- `set_watchdog_panic_disabled` (`mcp_server_link.py:290`) has no gate and no read-back.
- `safety_set_poll_period(0)`, `io_set_output` and `io_set_direction` are ungated.
- `control_set_zone_coupling` can raise IndexError on non-contiguous zone lists.
- The zone-convert precheck raises TypeError on a null `temp_source`; its worst case is about 290 s.
- `zone_current_sweep_abort` has no read-back.
- `profiles_save` resolves the host differently from its read-back.
- `kiln_config_apply` has no own mid-run check and relies on the firmware 409.
- LCD-19 (bench) starts a firing but is not `heat=True` in `registry.py`; see the estop section.

## INFO (checked, clean)

- Every MCP-level confirm gate uses `confirm is not True` before any I/O. Inner truthy checks (`coordinated_gpio_test.py:252`, `allow_stale` and similar) are masked by the facade's `coerce`, which refuses non-bool values for `confirm*`/`force*`/`allow_*`/`ack_*`; `test_gate_flag_strictness_partb.py` covers it.
- `bench_test_start` forwards all 11 `bench_test_run` parameters; `ota_matrix_start` forwards all 14 and answers a dry run or unconfirmed call synchronously; `build_kilnfw_start` forwards its parameters. `build_jobs` records any BaseException as failed; `wait_s` is clamped to 120.
- `latency_soak` caps the synchronous run at 240 s and is GET-only; its start twin caps at 24 h.
- TOTP and web-auth secrets come from environment variables only and are never echoed.
- Read-only tools checked (`get_readiness`, `boot_guard_get`, `nvs_list_keys`, `backup_export`, `control_get_zones`, `update_status`, `update_fetch_status`, `recovery_status`, `safety_get_*`, and others) issue only GETs, except `update_check` (M6).
- `flash_firmware` worst case is about 200 s.
- The control zone writers (`control_set_zone_type`, `_limits`, `_coupling`, `control_set_relay_type`) match their CLAUDE.md claims.

## Estop heat-only gate (origin/dev 378d76eab, 24a5f6116)

After these commits `estop_verified` not_done blocks only heat cases:
`HEAT_ONLY_BLOCKING_KEYS = {"estop_verified"}`, `BoardInfo.heat_blocked`,
`PreflightReport.ok_for_heat`. The runner sets
`ctx["estop_unverified"] = (not ok) or heat_blocked` (fail closed), forces
`allow_heat`, `lcd19_allow_heat`, `lcd22_allow_heat` and `ota_allow_heat` to
False, and SKIPs every `spec.heat` case with reason `estop_unverified`.
`cases_heat._capability_preflight_ok` also refuses on `heat_blocked`.

Every bench call site that can start a profile, an autotune or energize a
relay was traced:

| Site | Case(s) | `heat=True` | Other gates |
|------|---------|-------------|-------------|
| `cases_heat._start_bench_profile` (335) | HP-01..07 via `_hp_run`, `_case_hp04/05/06`, `_run_hp03_profile`, `_run_hp07_profile` | yes (all HP) | calls `_capability_preflight_ok` |
| `cases_autotune.py:271/326/410` | AT-01, AT-02, AT-04 | yes | |
| `cases_aux._start_and_confirm` (165) | AX-T01, AX-K01, AX-T02 (`_AX_HEAT`) | yes | |
| `cases_ota.py:1016` `_default_ote07_heat` | OT-E07, OT-G04 | yes | `_start_bench_profile` preflight, `ota_allow_heat` |
| `cases_ota.py:1053` autotune start | OT-E08 | yes | `A._at_preflight` calls `_capability_preflight_ok` |
| `cases_lcd.py:4129` | LCD-22 | yes | `allow_heat`, `lcd22_allow_heat`, `_capability_preflight_ok` |
| `cases_lcd.py:4706` `_lcd_edit_run` | LCD-23, LCD-24, LCD-25 | yes | same three gates |
| `cases_lcd.py` `_case_lcd10` (6247) | LCD-10 | yes | same three gates |
| `cases_lcd.py:3621` | LCD-19 stop_gated sub-check | **no** | `allow_heat` and `lcd19_allow_heat` (both forced off), `_start_bench_profile` preflight |
| `cases_web_safety.py:500` `probe=1` | WEB-RDY-04 | no (correct) | cannot start: firmware `profile_exec_start_post_handler` (`dashboard_exec_http.c`) runs the readiness gate, then answers 400 "id missing or invalid" (around 788-791) before any run |

No case energizes heat without being blocked when the E-stop is unverified:
no HIGH finding. LCD-19 is the one firing starter not flagged `heat=True`.
It is blocked by two layers (the forced-off opt-ins and the start helper's
preflight), but the `heat` flag also drives the run-level `allow_heat=False`
skip and the heat-last ordering. Flagging it would need its non-heat
sub-checks split out, so this is LOW. SP-09 is `heat=True` and operator-only.

Non-bench consumers (`run_queue.py`, the `capability_preflight` MCP tool, MCP
`profiles_start` / `autotune_start`) read `.ok`, not `ok_for_heat`, so they do
not refuse on an unverified E-stop. The firmware readiness gate still refuses
heat starts, so this is INFO.

## Tests I would want (not added)

1. `control_set_aux_manual`: `relay_state_unknown` / `i2c_failed` set must FAIL (H1).
2. Pico GPIO writers: OpenOCD command recorder, every sequence ends resumed, including when the read-back raises (H2).
3. Pico GPIO writers: ARMED or unreadable ARMED refuses with no probe I/O (H3).
4. `wifi_add_network` / `wifi_set_mode`: unreadable read-back must not start "ok" (M1; N5/N6 MISSED).
5. `thermo_write_reg`: read-back exception must FAIL (M2).
6. `capability_preflight`: crash_report 401/5xx/timeout and readiness 401 must not read as clean; missing `acknowledged` must not read as acknowledged (M3).
7. `profile_live_*`: a failed or unverified read-back result must not start "ok"; edit read-back compares the edited field (M4).
8. Adaptive tune: missing `enabled` / `revert_available` must not verify (M5).
9. `update_check`: assert method; whichever classification is chosen, pin it (M6).
10. Every MCP tool's default wait plus prechecks stays under 300 s (a registry-wide static test over defaults) (M7).
11. Safety writers' run gate: unknown executor/autotune state refuses (M8).
12. `backup_import`: a failed readiness re-read is reported as a failure (M9).

## Test runs

pytest (`uv run pytest -q tests -k "mcp or aux or control or update or recovery or flash or ota or bench_test or totp or web_auth or safety or build_job"`):
4165 passed, 14 skipped, 3146 deselected, 118 subtests passed in 589.89 s, exit 0.

`tools\negtest.ps1 -Preset pytest -Mutations` (baseline passed; real tree unchanged):

| # | Mutation | Result | Caught by |
|---|----------|--------|-----------|
| N1 | aux_manual gate made truthy | CAUGHT | `SetAuxManualTest::test_confirm_gate_exactly_true` |
| N2 | aux shadow check removed | CAUGHT | `test_shadow_mismatch_fails_loud` |
| N3 | preflight crash read fail-closed | CAUGHT | AllPresent / BenignMissing tests (fakes rely on fail-open) |
| N4 | profile_live read-back always OK | CAUGHT | profile_live tests |
| N5 | wifi_set_mode unreadable becomes FAILED | **MISSED** | none |
| N6 | wifi_add_network unreadable becomes FAILED | **MISSED** | none |
| N7 | recovery_pico_upload gate truthy | CAUGHT | `test_non_true_confirm_refuses_with_no_io` |
| N8 | update_stage_release sha check removed | CAUGHT | `test_stage_sha_mismatch_fails_loud` |
| N9 | bench_test_start passes allow_flash=False | CAUGHT | `test_every_gate_argument_reaches_the_runner_unchanged` |
| N10 | adaptive `enabled` default True | CAUGHT | `test_missing_fields_default_rather_than_raise` |
