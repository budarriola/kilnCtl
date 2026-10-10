# PcTools board-writing tools review, 2026-10-09

Sub-reviews: a41ff705 (safety/info), a7f72f6e (zones/aux), a1f89434 (ui_test items), plus the
tooling/checks sub-review. Part A = this document's author (files: control, zones, aux,
config_presets, ui_test, info, safety, thermo clients/servers). Part B appends its SHAs.

| Severity | Finding | Status |
|---|---|---|
| HIGH | ui_run_script applied the preset by default, no confirm, no run gate, result discarded | fixed in part A (apply_preset default False, confirm=True + run gate, partial apply raises) |
| MED | factory_default_then_load_preset result/exception handling, no run precheck | fixed in part A (precheck before erase, BOARD WIPED message for any failure/partial) |
| MED | zone_limits/zone_type re-post omit-preserved fields; regex missed hystc/coilpower | fixed in part A (single shared regex, body stripped) |
| MED | apply_preset partial-write reporting and PID read-back | fixed in part A (preset_partial note, PID read-back, RampAssist error handled) |
| MED | thermo writers lack tool-side confirm and run gate | fixed in part A (confirm is True + fail-closed run gate; write_reg read-back). Firmware mode gate: separate firmware task |
| MED | cfgfs_format stale "NVS stays authoritative" docstring | fixed in part A |
| MED | backup_import "may have committed" on transport failure, content read-back | deferred (not done in part A) |
| LOW | POST timeouts not reported as state UNKNOWN (zones, aux, safety_cfg) | fixed in part A |
| LOW | safety_clear_trip precheck/read-back, mask 1<<(reason-1), override | fixed in part A (allow_unexpected_mask) |
| LOW | safety_set_fault_out deassert needs confirm | fixed in part A |
| LOW | convert_onoff_zone_to_aux readiness re-read, collateral compare, 500 read-back | fixed in part A |
| LOW | profile_save_bench_aux_rule labels 5xx/timeouts as refused | fixed in part A |
| LOW | get_stored_relay_io_hits skips profile without segments | fixed in part A (fails closed) |
| LOW | client generation token for /api/zones | deferred: needs firmware |
| LOW | skip_backup / bench-test flag names bypass facade strict-bool gate | deferred: shared registry (part B territory) |
| LOW | kiln_config_apply id match, quarantine client docstring/uncaught errors, crash_report_clear timeout text, post_commissioning read-back, zone_current_sweep_start, ramp_assist/adaptive_tune/autotune_accept/ota_rollback_pico/safety_set_log_level read-backs, expander_* confirm, relay_cycles_kept text | deferred (not done in part A) |

Tests: tools/PcTools/tests/test_pctools_write_review_2026_10_09.py; negtest CAUGHT for the
ui_run_script confirm gate, the hystc/coilpower regex, and the thermo confirm gate.
