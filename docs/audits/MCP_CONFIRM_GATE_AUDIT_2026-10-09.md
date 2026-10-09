# MCP confirm-gate audit (2026-10-09)

Scope: every kilnctrl MCP tool registered from `tools/PcTools/src/kilnctrl/mcp_server*.py`
at origin/dev `1e6d375f` (224 decorated tools, plus 7 `mcpkit/workbench.py` build/test tools,
231 total). Audit only; no code was changed.

For each tool that mutates board state (POST, flash, reset, NVS write, relay write) four
things were checked:

- **(a) Gate.** Does it refuse unless `confirm is True` exactly? A truthy non-bool must not pass.
  Tools documented as ungated, such as `web_auth_logout`, are exempt.
- **(b) Read-back.** Does it verify by read-back where its docstring says it does?
- **(c) Credentials.** Does it ever print a credential?
- **(d) Mid-run.** Where it is documented to refuse mid-run, does it actually refuse?

Separately, docstrings were checked for claims the code does not back up.

## How the facade treats gate flags

`mcpkit/registry.py:529-557` (`_is_gate_flag_name`, `_gate_flag_problem`) refuses a non-bool
value for parameters named `confirm`, `force` or `allow_*`, when the schema type includes
boolean. Every other bool parameter goes through `_coerce_value` (`registry.py:559-570`), which
turns `"true"`, `"1"`, `"yes"`, `"on"` and `"high"` into `True`.

What this means for (a):

- **Through `kiln_call` / `kiln_batch`:** a `confirm` parameter is strict even when the tool body
  only checks `not confirm`.
- **Direct Python calls bypass the registry.** These callers include the bench_test runner,
  `ota_matrix`, and `cases_ota.py`'s `_ote11_tool(...)(confirm=True)`. For them, a
  `not confirm` body check accepts any truthy value, such as `"no"`, `1` or `[0]`.
- **Gate flags with other names get no strict check at all,** even through the facade. Examples:
  `confirm_erase`, `ack_*`.

## Gated tools

Gate column key:

- **exact**: the tool body checks `confirm is not True`.
- **truthy**: the tool body checks `not confirm`. Strict through the facade only (see above).

| Tool | def | Gate (check line) | Read-back | Mid-run refusal | Credentials | Notes |
|---|---|---|---|---|---|---|
| control_set_aux_output | aux.py:77 | exact (131) | yes | precheck + 409 | n/a | |
| control_set_aux_manual | aux.py:180 | exact (209) | yes | precheck + 409 | n/a | |
| control_convert_onoff_zone_to_aux | aux.py:243 | exact (281, 348) | yes on the normal path; **none on the `resume_relay` path** | precheck + 409 | n/a | F7 |
| profile_save_bench_aux_rule | aux.py:418 | exact (499) | yes | precheck | n/a | |
| control_set_zone_limits | control.py:621 | exact (735) | yes + collateral diff | precheck (fails closed) + 409 | n/a | |
| control_set_zone_type | control.py:844 | exact (917) | yes + collateral diff | precheck + 409 | n/a | |
| control_set_zone_coupling | control.py:1090 | exact (1194) | yes, tolerance 0.0005 (1082), matches docstring | precheck + 409 | n/a | |
| control_set_relay_type | control.py:1314 | exact (1376) | yes | precheck + 409 | n/a | |
| flash_recovery | flash.py:1648 | exact (1781) | yes | n/a | n/a | |
| estop_verify | info.py:416 | exact (497) | readiness re-read | refuses on any safety_trip that is not ok | n/a | |
| backup_import | info.py:1036 | exact (1139) | readiness before/after only (no content read-back is claimed) | board-side | n/a | |
| crash_report_ack | info.py:311 | truthy (377) | yes | n/a | n/a | F5 |
| crash_report_clear | info.py:536 | truthy (653); `allow_unacknowledged` strict through the facade | yes (record and image) | n/a | n/a | F5 |
| kiln_configs_quarantine_clear | info.py:716 | truthy (769) | re-probe | n/a | n/a | the status probe POSTs without `confirm`, which the board treats as non-mutating; F5 |
| kiln_config_apply | info.py:793 | truthy (837) | polls apply_status | board 428/409 | n/a | F5 |
| cfgfs_format | info.py:1426 | truthy (1483) | re-read: mounted, file_count 0 | n/a | n/a | F5 |
| network_set_ip_config | network.py:112 | exact (165) | yes (ip_mode + static fields) | precheck | no credential sent to an unconfirmed host | |
| recovery_enter | ota.py:223 | exact (246) | n/a (board 409) | board-side | n/a | |
| sw_reset_esp | ota.py:302 | truthy (355) | link back up | board-side | n/a | F5 |
| ota_matrix_run / ota_matrix_start | ota_matrix.py:339 / 452 | exact (424 / 481) | per-case | run-level gate | n/a | |
| recovery_exit, recovery_wifi_reset, recovery_boot_guard_reset, recovery_pico_upload, recovery_pico_abort, recovery_sw_reset, recovery_push_esp_image, recovery_apply_staged | recovery.py:386-925 | exact (404, 452, 501, 557, 673, 747, 808, 948) | status before and after | n/a (recovery image) | n/a | |
| totp_reset_password | totp.py:117 | exact (187) | yes (real login) | n/a | reads env only; never echoed | |
| update_stage_upload / update_stage_clear / update_stage_release / update_set_settings / update_fetch_cancel | update.py:90 / 236 / 359 / 452 / 510 | exact (167 / 252 / 395 / 469 / 525); `ack_no_safety` and the downgrade flags use `is True` | yes; `stage_release` checks verified header, github source and sha256 as its docstring says | board 409 | n/a | |
| adaptive_tune_set_enabled / adaptive_tune_revert | adaptive_tune.py:126 / 176 | truthy (155 / 196) | POST ack only; output says "confirmed by the board" | n/a | n/a | F5, F11 |
| ramp_assist_set_enabled | ramp_assist.py:79 | truthy (107) | POST ack only; output says "confirmed by the board" | n/a | n/a | F5, F11 |
| coordinated_gpio_test | coordinated_gpio_test.py:93 | truthy (136) | n/a | preflight (ARMED, run, interlock, link) | n/a | F5 |
| debug_program | debug.py:229 | truthy (262) | n/a | n/a | n/a | F5 |
| debug_write_memory | debug.py:611 | truthy (623) | n/a | n/a | n/a | F5 |
| profile_live_fork / profile_live_edit / profile_live_decide | profile_live.py:65 / 92 / 141 | truthy (79 / 126 / 163) | board response | board 409 | n/a | F5 |
| safety_set_rate_guard | safety.py:1204 | truthy (1245) | yes (`apply_safety_fields` verify) | precheck, **fails open** | n/a | F4, F5 |
| web_auth_setup | web_auth.py:81 | truthy (290) | yes (`admin_password_set`, `web_enabled`) | n/a | prints env-var presence only | F5 |
| zone_current_sweep_start | zones_current_sweep.py:86 | truthy (137) | n/a | board-side | n/a | F5 |
| flash_firmware erase path | flash.py:1017 | `confirm_erase` truthy (1330), **coerced by the facade** | yes (partition + fw_build) | none | none | F2 |

## Ungated mutating tools (no `confirm` parameter)

| Tool | def | Read-back | Mid-run refusal | Notes |
|---|---|---|---|---|
| factory_default_then_load_preset | ui_test.py:141 | waits for reboot; preset read-back when `host` is given | board mode gate only | **erases kiln_nvs (default) up to all three partitions**; F1 |
| flash_firmware | flash.py:1017 | yes | none | JTAG flash with no confirm; F2 |
| load_config_preset | config_presets.py:87 | yes when `host` is given | none in the tool | whole-page zones write plus PID gains and safety fields; F6 |
| safety_set_tc_type | safety.py:362 | yes | precheck, fails open | Pico flash write; F3, F4 |
| safety_set_commissioning_fields | safety.py:1034 | yes | precheck, fails open | Pico flash write, arbitrary fields; F3, F4 |
| control_set_zone_pid / control_set_zone_model | control.py:445 / 458 | none | none in the tool | F8 |
| ota_update_esp / ota_rollback_esp / ota_recovery_exit_esp / ota_update_pico | ota.py:120 / 165 / 263 / 375 | partial | board interlock | F9 |
| ota_rollback_pico | safety.py:1284 | n/a | board-side | F9 |
| profiles_start / profiles_save / profiles_delete | profiles.py:218 / 133 / 170 | n/a | board-side | `profiles_start` turns on heat; F9 |
| profiles_stop / profiles_pause / profiles_resume / profiles_ack_last_run | profiles.py:236-275 | n/a | n/a | safe-direction or operator controls; acceptable |
| autotune_start / autotune_abort / autotune_accept | autotune.py:95 / 167 / 180 | n/a | board-side | `autotune_start` heats, `autotune_accept` writes gains; `ack_unsettled` is coerced; F9, F10 |
| safety_request_enable / safety_clear_trip / safety_set_poll_period / safety_set_fault_out / safety_set_log_level | safety.py:237 / 297 / 262 / 275 / 319 | n/a | Pico-side | `safety_clear_trip` and `safety_request_enable` are safety-relevant; F9 |
| wifi_add_network / wifi_set_mode / wifi_set_ap_identity / wifi_forget | wifi.py:144 / 176 / 194 / 228 | none | n/a | passwords never echoed; F12 |
| set_watchdog_panic_disabled / restart_uart | link.py:290 / 245 | none | n/a | the panic-disable setting persists |
| debug_reset / debug_halt / debug_resume / debug_step | debug.py:321 / 471 / 501 / 512 | reset markers | n/a | |
| io_set_relay / io_set_relay_mask / io_all_relays_off / io_set_output / io_set_direction / io_set_auto_report / expander_* writes / expander_reset | io.py:89-309 | refusal-aware reply | firmware refuses relay writes during a run | relay 4 refused in the tool |
| thermo_config_channel / thermo_set_thresholds / thermo_set_cj_offset / thermo_one_shot / thermo_clear_faults / thermo_set_auto_report / thermo_write_reg | thermo.py:107-235 | none | n/a | `thermo_write_reg` is raw MAX31856 register writes |
| touch_inject / touch_set_tap_dump / ui_run_script / ui_step / press_button | touch.py:88, ui_test.py:84/114, actions.py:120 | n/a | n/a | `ui_run_script(apply_preset=True)` applies a preset |
| gpio_probe_set_mode / gpio_probe_write / pico_gpio_set_mode / pico_gpio_write | gpio_probe.py:78/109, pico_gpio_probe.py:84/108 | n/a | n/a | |
| fixture_set_relay / fixture_all_off | fixture.py:57 / 91 | n/a | n/a | fixture board, not the DUT |
| bench_test_run / bench_test_start | bench_test.py:28 / 226 | per case | per case | `allow_heat` defaults True; F13 |
| zone_current_sweep_abort | zones_current_sweep.py:214 | n/a | n/a | documented as safe at any time; exempt |
| web_auth_logout | web_auth.py:394 | n/a | n/a | documented as ungated; exempt |

## Findings

Severity is HIGH, MED or LOW. Lines refer to `tools/PcTools/src/kilnctrl/` unless a path says
otherwise.

**F1 HIGH: `factory_default_then_load_preset` erases NVS with no confirm gate.**
Status: FIXED (2026-10-09).
`mcp_server_ui_test.py:141`.

- The default scope is KILN, which wipes all of `kiln_nvs`: zones, relay names, rules,
  relay-cycle counters and run_state. Scope ALL also drops Wi-Fi and profiles.
- A single `kiln_call` with only a preset name destroys tuned configuration.
- Only the firmware mode gate stands in the way, and it applies during a run only.
- Every narrower writer in the same codebase, such as `control_set_zone_limits` or
  `cfgfs_format`, requires `confirm is True`.

**F2 MED: `flash_firmware` has no `confirm`, and its erase gate is not strict.**
Status: FIXED (2026-10-09).

- `mcp_server_flash.py:1017`: a JTAG flash of bootloader, partition table and app has no
  `confirm` parameter at all. `flash_recovery` (`mcp_server_flash.py:1781`) requires
  `confirm is True`.
- `mcp_server_flash.py:1330`: the `erase_partitions` gate is `not confirm_erase`.
  `confirm_erase` does not match the facade's gate-flag names (`confirm`, `force`, `allow_*`),
  so `confirm_erase="yes"` or `"1"` is coerced to `True` (`mcpkit/registry.py:529-530, 559-570`)
  and the partitions are erased.
- The docstring (`mcp_server_flash.py:1228-1240`) presents `confirm_erase` as the deliberate gate
  for an owner-decided destructive path.

**F3 MED: Safety-processor flash writers have no confirm gate.**
Status: FIXED (2026-10-09).
`safety_set_tc_type` (`mcp_server_safety.py:362`) and `safety_set_commissioning_fields`
(`mcp_server_safety.py:1034`) write RP2040 config flash with no `confirm`.
`safety_set_commissioning_fields` accepts arbitrary field names, including `abs_max_temp_c` and
`max_rate_c_per_min`. Their sibling `safety_set_rate_guard` writes a subset of the same fields and
refuses without `confirm` (`mcp_server_safety.py:1245`). Both docstrings stress "THIS WRITES THE
SAFETY PROCESSOR'S FLASH" but neither is gated.

**F4 MED: The safety-side mid-run check fails open.**
Status: FIXED (2026-10-09).
`_profile_or_autotune_running()` (`mcp_server_safety.py:1127-1149`) swallows
`ProfilesQueryError` and `AutotuneQueryError` (`pass` at 1141-1142 and 1147-1148) and returns
`None`, which means "not running".

- With the UART link down or slow, `safety_set_rate_guard`, `safety_set_tc_type` and
  `safety_set_commissioning_fields` proceed even though their docstrings say "REFUSES if a profile
  is firing ... checked BEFORE anything is sent".
- The Pico's own ARMED refusal is the only backstop.
- The control-side twin `_profile_or_autotune_running_reason()` (`mcp_server_control.py:484-510`)
  fails closed. The two disagree.

**F5 LOW: About 25 gated tools check `not confirm` instead of `confirm is not True`.**
Status: FIXED (2026-10-09).
These are strict only through the facade. A direct Python caller passing `"no"`, `1` or any
non-empty object passes the gate.

- `mcp_server_adaptive_tune.py:155, 196`
- `mcp_server_ramp_assist.py:107`
- `mcp_server_coordinated_gpio_test.py:136`
- `mcp_server_debug.py:262, 623`
- `mcp_server_info.py:377, 653, 769, 837, 1483`
- `mcp_server_ota.py:355`
- `mcp_server_profile_live.py:79, 126, 163`
- `mcp_server_safety.py:1245`
- `mcp_server_web_auth.py:290`
- `mcp_server_zones_current_sweep.py:137`

None of these docstrings claims "exactly", so this is inconsistency, not a false claim.
`estop_verify`, `recovery_*`, `update_*`, aux and the control writers already use the exact form.

**F6 MED: `load_config_preset` writes a whole page with no confirm and no mid-run precheck.**
Status: FIXED (2026-10-09).
`mcp_server_config_presets.py:87`.

- It writes PID gains over UART for every zone.
- With `host`, it also does a whole-page `/api/zones` write. With `safety_host`, it also writes
  Pico commissioning fields.
- CLAUDE.md records that an unverified preset restore once left bench zone 2 at `zone_type=1`.
- The narrow writers exist because this tool is too broad, yet the broad one is the ungated one.

**F7 MED: The aux convert resume path has no read-back, contradicting the module docstring.**
Status: FIXED (2026-10-09).
`mcp_server_aux.py:279-292`.

- The `resume_relay` branch POSTs `move_zone_to_aux` and returns "ok" on the firmware ack alone.
  It does no tool-side read-back of zones, the aux store or profile rules.
- The module docstring (`mcp_server_aux.py:5-8`) says "Every mutating tool ... fails loud when
  the read-back disagrees with what was written".
- The normal (non-resume) path does verify.

**F8 MED: `control_set_zone_pid` and `control_set_zone_model` have no gate, no mid-run precheck
Status: FIXED (2026-10-09).
and no read-back.** `mcp_server_control.py:445, 458`.

- These are UART writes of tuned gains and model with one-line docstrings.
- `control_set_zone_coupling`, which writes neighbouring fields of the same zones config, has an
  exact gate, a mid-run refusal and a collateral read-back.
- Whether the firmware's mode gate covers the UART CONTROL path was not verified here.

**F9 MED: Several consequential mutators are ungated, and their docstrings never say so.**
Status: DECIDED (documented ungated by design in each docstring) (2026-10-09).

- OTA: `ota_update_esp`, `ota_rollback_esp`, `ota_recovery_exit_esp`, `ota_update_pico`
  (`mcp_server_ota.py:120, 165, 263, 375`), and `ota_rollback_pico` (`mcp_server_safety.py:1284`).
- Heat start: `profiles_start` (`mcp_server_profiles.py:218`, turns on heat) and `autotune_start`
  (`mcp_server_autotune.py:95`).
- Stored config: `profiles_save` and `profiles_delete` (`mcp_server_profiles.py:133, 170`), and
  `autotune_accept` (`mcp_server_autotune.py:180`, writes gains).
- Safety link: `safety_clear_trip` and `safety_request_enable` (`mcp_server_safety.py:297, 237`).

Compare `recovery_enter` and `sw_reset_esp` (both gated) with `ota_rollback_esp` and
`ota_recovery_exit_esp` (not gated), which are the same class of reboot-into-another-image action.
If the owner intends these to be ungated, each docstring should say so, as `web_auth_logout`'s does.

**F10 LOW: `ack_*` flags are coerced, not strictly checked.**
Status: FIXED (facade strict-bool covers confirm*/force*/allow_*/ack_*) (2026-10-09).
Examples: `autotune_accept(ack_unsettled)` (`mcp_server_autotune.py:180`) and `kiln_config_apply`'s
`ack_hardware_differs` (`mcp_server_info.py:793`). These are acknowledgement gates, but their names do not match
`_is_gate_flag_name` (`mcpkit/registry.py:529-530`), so `"yes"` passes. `update_stage_release`'s
`ack_no_safety` is safe because its body uses `is True` (`mcp_server_update.py:404`).

**F11 LOW: "confirmed by the board" overstates what is checked.**
Status: FIXED (wording says POST accepted, or read-back added) (2026-10-09).
`adaptive_tune_set_enabled` (`mcp_server_adaptive_tune.py:172`) and `ramp_assist_set_enabled`
(`mcp_server_ramp_assist.py:121`) report "confirmed by the board" from the POST's own
`{"ok":true}`. No read-back happens. `ramp_assist`'s docstring itself admits that `ok:false` can
still mean the value applied live.

**F12 LOW: Wi-Fi passwords travel as tool parameters.**
Status: DECIDED (left as is, docstring note) (2026-10-09).
`wifi_add_network(password=...)` (`mcp_server_wifi.py:144`) and
`wifi_set_ap_identity(ap_password=...)` (`mcp_server_wifi.py:194`) take secrets as tool
parameters. The tools never echo them; the AP password is shown as `[set]`/`[unset]` at
`mcp_server_wifi.py:104`. However, MCP clients log call arguments. `totp_reset_password` and
`web_auth_setup` read secrets from the environment for exactly this reason. With `ssid=None`,
`wifi_add_network` loads saved credentials and does not expose them. No tool was found that
prints a credential value.

**F13 LOW: `bench_test_run` and `bench_test_start` default `allow_heat=True`.**
Status: DECIDED (left as is) (2026-10-09).
`mcp_server_bench_test.py:29`. Every other `allow_*` heat gate in this package defaults to False:
`ota_matrix_run`'s `allow_heat` and `bench_test_run`'s own `ota_allow_heat`. An agent running
`bench_test_run(suite=...)` with defaults gets the heat-marked cases. The docstring
(`mcp_server_bench_test.py:39-50`) does document this, so it is a consistency issue, not a false
claim.

**F14 LOW: `fixture_flash` is described as a tool but is not registered.**
Status: FIXED (fixture_flash is a Python helper; docstring and CLAUDE.md wording corrected, tool count unchanged) (2026-10-09).
`mcp_server_flash.py:1908` has no `@_core._tool()` decorator. Yet `flash_firmware`'s docstring
(`mcp_server_flash.py:1054`, "Flashing the fixture is `fixture_flash()`") and CLAUDE.md's flash
notes refer to it as if it were a callable tool. `kiln_call(name="fixture_flash")` fails.

## Checked and found consistent

- (b) Every read-back claim in the gated table above holds, except F7 and F11.
  - `control_set_zone_coupling`'s tolerance constant (`mcp_server_control.py:1082`, 0.0005)
    matches its docstring.
  - `update_stage_release` verifies the header, source and sha256 exactly as documented.
  - `web_auth_setup` reads back `admin_password_set` before enabling auth.
- (c) No tool returns a credential value.
  - `web_auth_setup` (`mcp_server_web_auth.py:184`) prints env-var presence booleans only.
  - `totp_reset_password` prints status codes only and restores the password env var after its
    verifying login.
  - `network_set_ip_config` refuses to send a credential to a host it cannot tie to the UART
    board.
- (d) Every tool documented to refuse mid-run does refuse when its status read succeeds. The only
  failure mode found is the fail-open read error in F4.
