# PcTools MCP tool test coverage sweep (2026-10-10)

Scope: every `tools/PcTools/src/kilnctrl/mcp_server_*.py` function with a `confirm` or
`confirm_erase` parameter at origin/dev `129586d4` (76 gate parameters, 75 tools; `flash_firmware`
carries two). Follow-up to `MCP_CONFIRM_GATE_AUDIT_2026-10-09.md` (F1-F13), whose fixes were
spot-checked here, not re-derived. Tests use fakes only; no board, no real network.

Checked per tool: (a) refuses a truthy non-`True` gate value, (b) refuses mid-run where documented,
(c) read-back mismatch fails loud, (d) no credential echoed.

## What was added

| Test | Covers |
|---|---|
| `tests/test_mcp_confirm_gate_sweep.py` | All 76 gate parameters, discovered by reflection (a new gated tool is picked up automatically). Runtime: `"yes"`, `1`, `"true"`, `[1]` make no non-GET HTTP request, spawn no subprocess, and reply with a refusal or a pre-gate read error (sockets, urlopen, subprocess are blocked). Static: the gate is an `is True` / `is not True` test (or a `*gate*`/`*refusal*` helper that is), never plain truthiness. `flash_firmware` runtime case is skipped (its gate sits after a build-output check; covered by `test_flash_firmware_confirm_gate.py`). |
| `tests/test_mcp_server_kiln_configs_tools.py` | `kiln_configs_quarantine_clear` and `kiln_config_apply` at tool level (previously only their HTTP clients were tested): dry run and truthy values never POST; success only after the re-probe reads not-quarantined; still-quarantined and re-probe-failed are FAILED/UNKNOWN, never ok; apply `diverged` fails loud even with `done_ok`; another id in apply_status is UNKNOWN; poll failure is UNKNOWN; mid-run 409 refuses without polling; hardware-differs 428 is never auto-acked. |

Negative test (`tools\negtest.ps1 -Preset pytest`, 4 mutations, all CAUGHT): quarantine gate `is not True`
to `not confirm`; quarantine read-back removed; `recovery_enter` gate to `not confirm` (caught by the
sweep, which sees a real POST); apply `diverged` check removed.

## Coverage table (keyword-level, then judged by reading the tool)

Columns: gate = exact `True` test (after the sweep, all pass); read-back = tool verifies by re-reading
(a=yes, ack=reports the board's POST ack only); tests = existing test files naming the tool
(excluding the new sweep).

| Area | Tools | Gate | Read-back | Mid-run | Existing tests | Gap |
|---|---|---|---|---|---|---|
| aux | control_set_aux_output, _manual, convert_onoff_zone_to_aux, profile_save_bench_aux_rule | exact | yes | precheck+409 | 1-3 files each | none found |
| control writers | set_zone_limits/type/coupling/relay_type/model/pid | exact | yes | precheck+409 | 1 file each | none found |
| info | backup_import, cfgfs_format, crash_report_ack/clear, estop_verify | exact | yes (backup_import: readiness only, as documented) | n/a | 1-3 | none |
| info | kiln_configs_quarantine_clear, kiln_config_apply | exact | yes / poll | board 409 | client only | FIXED here (tool-level tests) |
| flash | flash_firmware, flash_recovery | exact | yes | n/a | 13 / 1 | none |
| ota/recovery | recovery_enter, sw_reset_esp, recovery_* (8) | exact | recovery_enter: ack only (documented, no wait) | board 409 | 1-5 | LOW-2 |
| update | update_stage_*, update_set_settings, update_fetch_cancel | exact | yes | board 409 | 1-2 | none |
| safety | set_tc_type, commissioning_fields, rate_guard, fault_out | exact | yes | precheck | 2-3 | none |
| wifi | add_network, set_mode, forget | exact via `_wifi_write_refusal` | none (doc says none) | precheck | 1 | none |
| totp/web_auth | totp_reset_password, web_auth_setup | exact | login re-verify / read-back | n/a | 2 each | none; env-only credentials, outputs checked |
| ramp_assist / adaptive_tune | set_enabled | exact | yes | n/a | 1-2 | none |
| adaptive_tune | adaptive_tune_revert | exact | NONE | n/a | 1 | FINDING F1 |
| profile_live, io expander_*, thermo_*, pico_gpio_*, fixture, debug_*, coordinated_gpio_test, ota_matrix_*, network, zone_current_sweep, ui_test, update | -- | exact | per tool | per tool | 1+ | none beyond the sweep |

## Findings (tool code not changed)

- **F1 MED: `adaptive_tune_revert` reports "reverted" on the POST ack alone.**
  `mcp_server_adaptive_tune.py` ends with `return f"ok - zone {zone} adaptive-tune refinement reverted"`
  after only `result.get("ok")`. It rewrites a zone's PID gains, and its sibling
  `adaptive_tune_set_enabled` already re-reads and reports FAILED/UNVERIFIED. Fix: re-read
  `adaptive_tune_get_status` (`revert_available` should drop, gains return to `prior_k_dc`) or the
  zones gains, and fail loud otherwise. (Carried over from audit F11, which fixed only set_enabled.)
- **F2 LOW: `recovery_enter` claims `ok` on the board's ack** with no follow-up read. Documented
  ("does not wait for the reboot") and the board does the refusing (409), so not a lie, just
  unverified; `recovery_status` is the follow-up.
- **F3 LOW (test-design note, not a defect): gate runs after a read in several tools** (`cfgfs_format`,
  `crash_report_*`, `kiln_config_apply` aside, `update_*`, ...): they GET before checking `confirm`. No
  write happens before the gate (the sweep asserts this); only the reply differs when the board is
  unreachable ("could not read" instead of "refused"). `flash_firmware` checks build outputs before
  its gate for the same reason.
- No tool was found that writes without a real `True`, and no tool echoed a credential in the reviewed
  outputs. All F1-F10 fixes from the 2026-10-09 audit hold.

## Not covered by this sweep

Read-back-mismatch and mid-run tests exist per tool only where listed above as "1+ files"; the sweep does
not generically assert them (each tool's read-back shape differs). A tool added with a `confirm` gate is
auto-covered for (a) only.
