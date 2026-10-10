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

## Part B and batch C closure (INFO-1 of REVIEW_TOOLINGB_WEBAUTH_2026-10-09.md)

Part B findings were fixed in 0c2846fb. Batch C follow-ups, all FIXED:

| Finding | Status |
|---|---|
| backup_import transport wording ("may have committed" only when it could have) | FIXED in 5188806e1 |
| crash_report_clear POST timeout reported as state UNKNOWN | FIXED in 5188806e1 |
| quarantine client uncaught OSError | FIXED in 5188806e1 |
| kiln_config_apply must check the status id | FIXED in 143d0e9fb |
| expander_* raw writers: confirm is True plus run gate | FIXED in 143d0e9fb |
| safety commissioning read-back failure after POST reports state UNKNOWN | FIXED in f5500bcfa |
| backup_import content read-back | skipped: a generic diff is unsound under merge and mirror semantics |
| client generation token for /api/zones | deferred: needs firmware |

Review REVIEW_TOOLINGB_WEBAUTH_2026-10-09.md items:

| Finding | Status |
|---|---|
| MED-1 flash_firmware link-down latched trip | FIXED in 9750dc627 (a latched trip is a note in link-down mode while not ARMED; running profile, autotune and energized relays stay hard refusals) |
| LOW-1 FL-10/FL-11 allow_flash exact True | FIXED in 22a8ed016 |
| LOW-2 running guards fail open (debug, wifi) | FIXED in 11a9a1f50 and 15f8f1682 (unreadable refuses, allow_running=True overrides; autotune covered; debug_step and leave_halted reads guarded; debug_resume left unguarded on purpose because a halted ESP cannot answer the state read) |
| LOW-3 read-backs that cannot fail | FIXED in 11a9a1f50, 15f8f1682, f313bfe15 |
| LOW-4 PcTools part (real tool through the registry) | FIXED in f313bfe15 |
| LOW-5 zones Save reload (web JS) | skipped: web JS item |
