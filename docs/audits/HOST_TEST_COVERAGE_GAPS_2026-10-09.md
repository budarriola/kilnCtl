# Host test coverage gaps, KilnFW and SaftyFW (2026-10-09)

Base: origin/dev at mint time. Complements `HOST_TEST_GAP_AUDIT_2026-10-09.md`
(which audits branches of recently changed code); this is a whole-tree map.

Method: a script classified every `.c` under `firmware/KilnFW/App` (307 files)
and `firmware/SaftyFW/src` (58 files). "Compiled" = the basename appears in
`App/test/build_host_tests.ps1` or `SaftyFW/test/build_host_tests.ps1`, or a
`test_*.c` `#include`s it. "Called" = an identifier equal to a non-static
function name appears in any `test_*.c` or fake. Both are heuristics: a name in
a script comment counts as compiled, a name in a stub counts as called. No
coverage instrumentation was run. Treat the numbers as a map, not a measurement.

Totals: KilnFW 237 of 307 compiled by name (70 not), SaftyFW 47 of 58 (11 not).
Most of the 70 are LVGL/panel UI pages, which are skipped here.

Key caveat for SaftyFW: `safety_core.c`, `link_task.c`, `thermo_task.c`,
`watchdog_task.c` and `discrete_task.c` are NOT host-compiled. The many
`test_safety_core_*.c` and `test_estop_deenergizes_relay.c` tests read the
source as text (`read_src("../src/tasks/safety_core.c")`) and assert that call
sites exist. They prove wiring is present, not that the guards behave. Behavior
is tested only for logic already extracted into host-compilable policy files
(`link_task_*`, `relay_grace`, `clock_health`, `watchdog_gate`, ...).

## Ranked table

Risk: S safety guard, R relay/heater drive, P persistent storage write,
N network input parsing. Tested: Y yes, T thin (under half of public functions
referenced), X no execution test (none, or source-text only).

| # | Module | What it does | Risk | Tested | Untested functions / branches |
|---|---|---|---|---|---|
| 1 | SaftyFW `src/tasks/safety_core.c` (1952 L) | Guard loop: trips S1..S14, relay command, trip latch, clear | S R | X (text-grep tests only) | Whole tick: each guard's trip/clear threshold, trip_mask derivation, relay de-energize order, clear_trip binding |
| 2 | SaftyFW `src/tasks/link_task.c` (3592 L) | kilnlink RX/TX, frame dispatch, SET_PARAM, clear-trip, update commands | S N P | X | Dispatch table, CRC/length reject paths, commit/reject glue, TC type gate wiring (only extracted `link_task_*` helpers run) |
| 3 | SaftyFW `src/tasks/thermo_task.c` (748 L) | MAX31856 read, fault bits, DRDY recovery feeding safety | S | X | Fault-bit to guard mapping, stale-sample handling, retry path |
| 4 | SaftyFW `src/tasks/watchdog_task.c` (344 L) | Check-in aggregation, hardware watchdog feed | S | X | Missed check-in -> no-feed branch (only `watchdog_gate` runs) |
| 5 | KilnFW `drivers/safety/safety_link_payload.c` (337 L) | Builds status/trip/diag/stats payloads for the link | S N | T (0/5 referenced; compile test only) | `safety_link_build_status_payload`, `_trip_event_`, `_diag_`, `_stats_`, `_fw_version_` byte layouts |
| 6 | KilnFW `drivers/safety/danger_mode.c` (378 L) | Operator danger-mode heat enable window | S R | T (1/9) | `request_start`, `set_heat_enable_request`, `remaining_ms` expiry, `stop`, `get_relay_status`, `init` |
| 7 | KilnFW `drivers/owners/kiln_io_owner.c` (938 L), `kiln_io.c` (523 L) | SX1509 relay/IO command owner, relay writes | R S | T (6/18, 7/15) | `command_set_relay`, `set_io_dir`, sx reset/read/scan, irq paths |
| 8 | KilnFW `drivers/http/aux_outputs_http.c` (211 L) | Spare-relay aux outputs GET/POST (drives relays) | R N P | X (no test names it) | Whole handler: field parse, range refusal, mode-gate 409, readback |
| 9 | KilnFW `http/zone_aux_convert_http.c` (179 L), `http/dashboard_exec_http.c` (914 L) | Zone-to-aux convert; exec pause/ack, safety clear trip, history CSV | R P N S | T (convert only via test_zones_http; exec 2/13) | `profile_exec_pause_post_handler`, `dashboard_safety_clear_trip`, `profile_exec_ack_last_run_post_handler`, firing_history / history_csv handlers |
| 10 | KilnFW `http/dashboard_autotune_http.c` (575 L) | Autotune start/abort/accept/status/trace | R P N | T (1/6) | `autotune_start_post_handler` gating, `_abort_`, `_accept_` (writes gains), trace CSV |
| 11 | KilnFW `http/diagnostics_http.c` (2302 L, 26 handlers) | Crash report ack/clear, estop verify, NVS keys, cfgfs, coredump | P N S | T (test_diagnostics_http.c, 2026-10-09) | `estop_verify_post_handler` refusal rules, crash_report_clear, nvs_keys kiln_auth refusal |
| 12 | KilnFW `http/dashboard_http.c` (990 L) | Dashboard status, relay POST | R N S | T (2/7) | `dashboard_set_relay`, `dashboard_http_get_safety_trip`, `_estop_asserted` |
| 13 | KilnFW `control/adaptive_tune_model.c` (827 L) | Adaptive tuning, writes zone gains | P R | T (3/12) | `apply_zone_plan`, `apply_coupled_plan`, `commit_*_locked`, breadcrumb mid-solve recovery |
| 14 | KilnFW `persist/cfg_fs_mount.c` (607 L) | LittleFS mount, format gate | P | T (6/13) | `format_*` state accessors, confirmation-pending path, auto-format deferral |
| 15 | KilnFW `persist/touch_cal_store.c` (243 L) | Touch calibration NVS store | P | X (only test_touch_dev names it) | load/save/CRC/version reject |
| 16 | KilnFW `http/ota_http_esp.c` (849 L), `update/update_http.c` (744 L) | OTA push, rollback, update gates | P N S | T (2/5 each) | `ota_esp_post_handler`, `ota_esp_rollback_post_handler`, `update_http_gate_refuses`, `_mode_gate_refuses`, `_stale_stage_check` |
| 17 | KilnFW `bridge/uart_bridge.c` (566 L) and `uart_bridge_{safety,system,ext_autotune,ext_wifi,io,info,thermo}.c` | PC-ESP UART command bridge: relay set, safety ops, autotune, wifi | R S N P | T (bridge 1/15; sub-files not compiled) | `bridge_args_ok`, `bridge_clamp_auto_period`, f32/lstring codecs, per-command arg validation |
| 18 | KilnFW `http/security_backend_web_auth.c` (534 L), `web_auth_session_status_http.c`, `dashboard_settings_http.c`, `setup_progress_http.c`, `dualwrite_window_http.c`, `cfg_fs_format_http.c` | Web auth backend, settings POST, format confirm | N P S | X / T | session issue/verify paths, settings field parse, format_confirm gating |
| 19 | KilnFW `net/wifi_prov_api.c` (1013 L) | Wi-Fi provisioning API | N P | T (14/36) | `do_scan`, `do_set_ap_password`, `do_set_ap_ssid`, `do_get_saved_networks`, ip mode |
| 20 | KilnFW `safety/rtc_watchdog.c` (40 L), `control/ct_leak_alarm_service.c` (121 L) | RTC watchdog, CT leak alarm service | S | X / T (1/2) | all of rtc_watchdog; alarm raise/clear in ct_leak service |
| 21 | KilnFW `persist/backup_json.c` (198 L) | JSON scanner used by backup import | N P | T (1/8) | `obj_find`, `field_num/_str`, `skip_value`, `arr_next`: malformed, nested, escaped-string, truncation |
| 22 | KilnFW `safety/safety_link_frame.c` (161 L) | Link frame pack/unpack helpers | S N | T (4/9) | `safety_read_u16/u32/f32_le`, `safety_put_*` endianness edges |
| 23 | KilnFW `main.c`, `main_network_http.c`, `main_bridges_bringup.c`, `monitor_task.c` | Boot sequencing, boot_guard confirm task | S P | X | boot_guard recovery gating of task start (hardware ordering, hard to host-test) |
| 24 | KilnFW `hw/SX1509.c` (1051 L) | I/O expander driver | R | X | register sequencing; testable via fake I2C |
| 25 | SaftyFW `update/saftyfw_image_identity_record.c`, `tasks/stack_margin_poller.c`, `log_task.c`, `discrete_task.c`, `main.c` | Image identity, stack poller, logging, discrete inputs (estop), init | S | X | identity record build/verify; discrete debounce |
| 26 | KilnFW `ui/lvgl_port.c` (5/16), `ui/ui_lcd_lock.c` (3/7), `sim/sim_backend.c`, `owners/thermo_owner.c` (2/11) | UI port, LCD lock, sim backend, thermo slots | R S | T | `ui_lcd_lock` force_lock / note_activity; thermo_owner slot accessors |

Low risk or skipped: `ui_page_*`, `ui_topbar`, `panel_spi*`, `NS2009.c`,
`ui_theme`, `ui_confirm`, `i2c_scan`; `web_encoding.c` is fully referenced.

## Top 10 recommended test campaigns

1. **safety_core guard behavior harness (SaftyFW).** Compile `safety_core.c`
   behind existing stubs (relay_owner via fake_gpio, FreeRTOS min stub) and
   drive the tick with synthetic temps, CT and link states. For each guard
   assert the trip reason, `trip_mask == 1 << (reason-1)`, relay de-energized
   in the same tick, latch survives sensor recovery, and clear-trip is refused
   while the cause persists. Replaces the source-text tests as the evidence.
2. **link_task frame dispatch fuzz (SaftyFW).** Feed truncated, bad-CRC,
   oversize, replayed-seq and wrong-boot-id frames through a fake UART. Assert
   no state change on rejects, correct NACK codes, SET_PARAM range refusal, and
   clear-trip / update commands refused when armed.
3. **thermo_task to guard path (SaftyFW).** With `fake_spi` MAX31856, inject
   open-circuit, out-of-range, stale DRDY and CRC faults. Assert the sample is
   flagged invalid and safety_core sees the fault rather than a last-good value.
4. **watchdog_task starvation (SaftyFW).** Skip one required check-in and
   assert the feed is withheld within the bound; skip none and assert feeding.
   Cover the boot-checkin list so a new task cannot be omitted silently.
5. **safety_link_payload golden byte layouts (KilnFW).** Golden vectors for
   status, trip_event, diag, stats and fw_version payloads, plus a round trip
   against the SaftyFW `link_frame` parser so a one-sided field change fails
   (reset-one-side class). Include `safety_link_frame.c` endian helpers at
   boundary values.
6. **danger_mode lifecycle (KilnFW).** Fake clock: request_start, touch
   extends, expiry at the exact ms, stop clears the heat request,
   set_heat_enable refused outside the window, init rebinds the safety class,
   get_relay_status mirrors the link state.
7. **kiln_io_owner / kiln_io relay command path (KilnFW).** Fake I2C SX1509:
   `command_set_relay` writes the expected bit, refuses when the owner is not
   ready or the mode gate is closed, a failed write leaves state unchanged and
   is reported. Cover set_io_dir and sx reset error returns.
8. **Relay/aux/exec HTTP handler matrix (KilnFW).** One harness over
   `aux_outputs_http`, `zone_aux_convert_http`, the `dashboard_http` relay
   POST, `dashboard_exec_http` pause/ack/safety_clear_trip and
   `dashboard_autotune_http` start/abort/accept. Bad, missing or oversize
   fields give 400, the system-mode gate gives 409, auth-tier refusal holds,
   success reads back, and no persistent write happens on any refusal (count
   stub store writes).
9. **(diagnostics_http part done 2026-10-09, test_diagnostics_http.c; ota_http part done 2026-10-10, test_ota_http_refusals.c; update_*_http gate refusals done 2026-10-10, test_update_http_refusals.c; item closed.) diagnostics_http and ota/update handler refusals (KilnFW).** Compile
   `diagnostics_http.c` with stubs: estop_verify refused unless safety_trip is
   ok, crash_report_clear refused when unacknowledged, nvs_keys refuses
   `kiln_auth`; ota_esp / rollback and `update_http_*gate_refuses` refuse during
   a run; streamed JSON respects chunk limits.
10. **Persistence stores and parsers (KilnFW).** `backup_json` malformed,
    nested, escaped and truncated input with fuzz seeds; `touch_cal_store`
    CRC / version / short-blob reject with load returning defaults;
    `cfg_fs_mount` format-pending and confirmation state machine;
    `adaptive_tune_model` commit paths with injected save failure (RAM
    unchanged after a failed save); `uart_bridge` arg validation and clamp
    helpers.

Not recommended: host tests for boot sequencing (`main*.c`) and SX1509
register timing beyond a fake I2C; these are hardware-ordering problems better
covered by bench cases.
