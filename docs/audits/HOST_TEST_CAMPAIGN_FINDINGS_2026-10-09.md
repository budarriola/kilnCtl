# Host test campaign findings, 2026-10-09

Entries are appended by campaign agents. Defects found while writing host tests;
the failing cases are NOT committed (the test file carries a comment naming the finding).

## Campaign 7: kiln_io.c / SX1509.c relay command path

Test: `firmware/KilnFW/App/test/test_kiln_io_sx_fake.c` (real `kiln_io.c` + real
`SX1509.c` over a register-level fake SX1509 behind `i2c_master_transmit*`).
Not testable on host: owner-task queue/lock interleaving (FreeRTOS stubs never deliver).
All three confirmed by probe runs of the fake.

### K7-01 (MEDIUM) FIXED in 501bb0d35: write lands, transfer reports error, coil left energised but module says OFF
- Where: `drivers/hw/SX1509.c` raw write / `write16_verified` (shadow updates only on ESP_OK); `drivers/owners/kiln_io.c` `kiln_io_set_relay` error path.
- Input: `kiln_io_set_relay(io, 1, true)` while the bus lands the write then reports `ESP_ERR_TIMEOUT` (lost ACK).
- Observed: returns an error, `relay_shadow == 0`, chip relay 1 energised.
- Expected: shadow reflects the chip (or a safe-off write is attempted) so module state never says OFF while a coil is driven.

### K7-02 (LOW/MEDIUM) FIXED in 501bb0d35: stuck-high latch bit: OFF fails read-back, shadow says OFF
- Where: `kiln_io.c:142` `kiln_io_resync_relay_shadow` (adopts driver data shadow, not a chip read-back), called from the failure paths at `kiln_io.c:302`/`:328`.
- Input: relay 1 on, latch bit 0 stuck high, `kiln_io_set_relay(io, 1, false)`.
- Observed: `ESP_ERR_INVALID_RESPONSE` returned, `relay_shadow == 0`, chip still energised.
- Expected: resync reads the chip, so `relay_shadow` reports ON.

### K7-03 (HIGH) FIXED in 501bb0d35: ON after expander soft reset reports success but drives nothing
- Where: `SX1509.c` `SX1509_reset` (all pins back to input, POR shadows); owner `CMD_SX_RESET` at `kiln_io_owner.c:554` sets `relay_shadow = 0` and does not re-drive dir/latches.
- Input: `SX1509_reset(&exp, false)`, then `kiln_io_set_relay(io, 1, true)`.
- Observed: returns `ESP_OK`, `relay_shadow == 1`, chip dir `0xFFFF` (relay pins inputs), relay not energised.
- Expected: relay pins re-configured as outputs after reset (re-init), or the command fails; never OK with nothing driven.

### K7-04 (LOW) FIXED in 501bb0d35: after chip POR, `kiln_io_all_relays_off` returns an error and never repairs direction
- Where: `kiln_io.c` all-relays-off path versus a stale dir shadow.
- Input: relay 1 on, chip POR behind the driver's back, `kiln_io_all_relays_off`.
- Observed: `ESP_ERR_INVALID_RESPONSE` (264), chip dir `0xFFFF` left as POR default.
- Expected: either ESP_OK with outputs re-driven off, or a re-init. (Pins as inputs are de-energised, so this is a safe state, only reported as failure and not repaired.)

## Campaigns 5 and 6: safety_link payload builders, danger_mode

### F5-1 (LOW) FIXED in 6d4cc7ab6: fw_version worst-case length comment miscounts

- File: `firmware/KilnFW/App/drivers/safety/safety_link_payload.c:252`
- Input: `safety_link_build_fw_version_payload` with a 64-byte commit and a
  32-byte datetime.
- Observed: payload is 108 bytes (12 fixed bytes + 64 + 32). Golden test
  `test_golden_fw_version_payload` pins 108.
- Expected per comment: "9 + 64 + 32 = 105 bytes".
- Impact: comment only. The output buffer is sized well above 108, so there
  is no overrun. Anyone sizing a receiver from the comment would be 3 bytes
  short.
- Test status: the test asserts the real value (108); nothing left out.

### F6-1 (LOW) FIXED in 6d4cc7ab6: re-entering danger mode clears heat_requested without releasing the wire request

- File: `firmware/KilnFW/App/drivers/safety/danger_mode.c:90` (inside
  `danger_mode_request_start`, `s_dm.heat_requested = false;`)
- Input: window open, `danger_mode_set_heat_enable_request(true)` succeeded
  (REQUEST_ENABLE(true) sent), then `danger_mode_request_start()` called again
  (for example a second operator click on Start).
- Observed: `danger_mode_get_heat_requested()` returns false and the tile shows
  heat off, but no `safety_link_request_enable(false)` is sent, so the Pico
  still holds the enable request until the window ends or Stop.
- Expected: display and wire agree, either by keeping `heat_requested` on
  re-entry or by sending the release.
- Impact: the display disagrees with the last request sent, which is the
  failure mode the field's own comment says it exists to prevent. The
  SaftyFW guards still decide whether K4 closes, and the window still times
  out and releases.
- Test status: the current behaviour (flag cleared, no release) is NOT
  asserted either way; `test_lifecycle` only checks the flag. Left out of the
  assertions pending an owner decision on the intended behaviour.

## Campaign 9: `diagnostics_http.c` (test_diagnostics_http.c)

No defect found. 147 checks across crash report get/ack/clear, estop verify,
coredump info/chunk, watchdog and ramp-assist POST, relay-cycles reset/restore,
danger mode start/stop/relay/enable, cfgfs status/file GET/POST, nvs keys and
route registration all matched the handlers' documented behaviour.

Scope notes, not defects:

- The estop_verify handler has no refusal rules of its own. The "refused unless
  safety_trip is ok" and `confirm` rules live in the MCP tool, so the handler
  test only checks that it calls `estop_verification_confirm()` and reports its
  result honestly.
- The v3-crash-record-after-upgrade scenario (DEV_FIRMWARE_REVIEW_14 MED-1) is
  decided in `crash_report.c`'s loader, not in these handlers. The handlers only
  render whatever `crash_report_get()` returns, so it is not reachable here.

## Campaign 8: relay / aux / profile-exec HTTP handlers

Tests: `test_aux_outputs_http_handlers.c` (128 checks; real `aux_outputs_cfg.c`, `aux_outputs_http_core.c`, `aux_outputs_http.c` and `system_mode_gate.c`, handlers fetched through the real `kiln_http_register` path, fake relay board) and `test_dashboard_exec_http_handlers.c` (39 checks; real `dashboard_exec_http.c`, GET paths link-stubbed, executor/readiness/danger-mode fakes that count calls). Both assert module state (store entries, enabled mask, rev, relay-board call log, executor call counts) on every refusal. Negtests: removing the aux mode gate and removing the start handler's `danger_mode_active()` refusal were both CAUGHT.

No defects found in the covered surface: mode gate 409 mid-run (profile and autotune), ADMIN tier for all three aux routes, relay 0/5/negative/overflow/non-numeric, truncated bodies, recv errors, content_len edge values, duplicate keys (first wins), claim-busy 409, zone-conflict 409, quarantine 409, disabling an aux drives its relay OFF, relay-board errors map to refusals with no store change, start id range and executor-refusal relay.

Observations (design notes, not filed as defects):
- K8-01 (INFO): `POST /api/profile_exec/start` has no `confirm` field and its only mode gate is the recovery-mode check plus the readiness interlock and `danger_mode_active()`; a second start while a run is active is refused only by `profile_executor_run()` (faked here), so that refusal is not covered at the handler level.
- K8-02 (INFO): stop/pause/resume/ack are deliberately ungated (stop works in recovery mode and with danger mode on); pinned by test.
- K8-03 (INFO): the raw `relay_post_handler` route no longer exists; the raw write path is aux manual via `dashboard_set_relay()`, covered here with the board faked. `zone_aux_convert_http.c` `move_handler` (hook on POST /api/zones, not a route) is not covered at handler level; its core has its own test.

## Campaign 10: persist parsers and stores (backup_json, touch_cal_store, pref_cfg_fs, ct_verify_store)

Test: `firmware/KilnFW/App/test/test_persist_campaign10.c` (129 checks, one
executable, wired into `build_host_tests.ps1`, expected count 75 -> 76). Real
modules over `fake_kv` and a real `cfg_fs` scratch directory, with the
`persist_scratch` OOM hook. Each defect below is pinned by a
"CHARACTERIZATION K10-xx" check that asserts today's behavior; when a fix
lands, invert that check. `backup_import.c` was out of scope (another agent).

**Status: K10-01..K10-14 (incl. 08b, 09b) FIXED in fd18342d6.** Every CHARACTERIZATION check was inverted (test now 144 checks). `backup_import.c` gained two "name too long or malformed" rejections as a consequence of the strict `backup_json_field_str`; `zones_current_sweep_task.c` stores 0 for non-finite CT verdict floats (K10-12).

### K10-10 (HIGH) pref_cfg_fs_resolve: transient OOM reading the file overwrites a NEWER file with older NVS

- File: `firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c`, `load_raw_impl` and `pref_cfg_fs_resolve`
- Input: a file above `PREF_CFG_FS_MAX_ITEM` (128 B) at rev 9 and an NVS copy at rev 3; the scratch allocation for the file read fails once.
- Observed: the read reports "no valid file", resolve adopts the NVS value, returns success and rewrites the file at rev 3. The rev-9 user data is gone.
- Expected: an allocation failure is an error; resolve must not touch the file.
- Test: `resolve returned success` then file reads rev 3.

### K10-09 (MEDIUM) pref_cfg_fs_load_raw / load_var report alloc failure as "absent"

- Same file. The functions have no error channel: `valid=false, rev=0` is identical for "no file" and "could not allocate". K10-09b is the same for `load_var`. Every caller that treats "absent" as "adopt defaults / NVS" inherits K10-10.

### K10-11 (MEDIUM) pref_cfg_fs_probe_newer_wrong_size answers "not newer" on alloc failure

- A newer-firmware file of the wrong size is detected normally; on OOM the probe returns false, so the caller treats the file as ordinary garbage and may overwrite it.

### K10-13 (MEDIUM) ct_verify_store_save serves an unpersisted verdict after a failed write

- File: `drivers/persist/ct_verify_store.c`. After `pref_cfg_fs_commit` fails the call returns an error but RAM already serves the new verdict (and rev is correctly not advanced). A reboot silently reverts to the old verdict. Callers that ignore the return value show a verification result that does not survive.

### K10-14 (MEDIUM) ct_verify_store_start: corrupt newer file silently replaced by older NVS

- A bad file (valid-looking size, failed validate) loses to the older NVS blob, and the file is overwritten. Nothing is reported beyond a log line. Same shape as K10-10 without needing OOM.

### K10-01 (MEDIUM) backup_json_field_str silently truncates an over-long string

- A 8-char value into a 4-byte buffer returns true with "abc". A backup carrying a long name/SSID is restored shortened with no error.

### K10-02 (MEDIUM) backup_json_field_str accepts an unterminated string

- `{"s":"abc` (truncated backup) returns true. Same silent-data-loss class as K10-01.

### K10-07 (LOW-MEDIUM) touch_cal_store_save has no read-back

- A write that reports OK but persists nothing returns `ESP_OK`; the board loses calibration at reboot with no indication. Failed `set_blob` and failed commit are reported correctly.

### K10-08 (LOW) touch_cal_store accepts NaN / all-zero coefficients (no CRC, no value validation)

- A NaN coefficient or an all-zero v1 record loads as "calibrated" and yields a degenerate touch map (K10-08b).

### K10-06 (LOW) touch_cal_store_load maps a corrupt NVS record to ESP_OK uncalibrated

- A `HAL_IO` read is indistinguishable from a first boot; the caller cannot tell data loss from a fresh board.

### K10-12 (LOW) ct_verify_blob_validate does not check the verdict floats

- NaN `measured_a` / `threshold_a` pass validation.

### K10-03 (LOW) backup_json \u escapes are mangled

- `"AB"` decodes to `Au0042` with no error. Only matters if a backup string can contain non-ASCII.

### K10-04 (LOW) backup_json_field_num accepts hex floats (strtod syntax)

- `0x1F` parses as 31. Not valid JSON; harmless for self-produced backups.

### K10-05 (LOW) backup_json_field_num ignores trailing garbage

- `12abc` parses as 12.

### Verified clean

- backup_json: nesting (10000 deep, iterative), commas inside strings, truncated objects/arrays terminate, nan/inf/null/string rejected, absent vs malformed optional distinguished.
- touch_cal_store: wrong version, version 0, truncated and oversized records all give uncalibrated; failed set_blob and failed commit are reported; fit rejects collinear and n<3.
- pref_cfg_fs: size mismatch both directions, validator rejection, truncated and oversized file on disk, write failure leaves the previous file, resolve tie-breaks (file higher, equal rev NVS wins and resyncs, NVS invalid uses file), save-side alloc failure reports `ESP_ERR_NO_MEM`, resolve scratch OOM reports failure and leaves the file.
- ct_verify_store: every validate field (size, version, zone_count, reserved, fingerprint, verdict, responded_ch), NVS truncated/newer/corrupt gives no verdict, never a pass; file beats stale NVS; invalid blob refused without touching RAM; rev not advanced by a failed save.

### Negative tests

Three mutations, all CAUGHT with a passing baseline (`tools
egtest.ps1 -Command` over `build_host_tests.ps1 -Only persist_campaign10`): touch_cal version check disabled (2 failures), ct_verify zone_count bound loosened (1), backup_json isfinite check dropped (2).

## Campaign 9b: OTA HTTP handlers (test_ota_http_refusals.c)

127 checks, built on `test_ota_http.c`'s stubs (extended with settable partition,
`esp_ota_*`, recovery-mode, relay-shadow and recv fakes). Covered: the
interlock gate (409 with a safety link, 428 without the acknowledge header),
the single update claim (a refusal or failure must release it), ESP push body
and size validation, the 500 mapping for no OTA partition, `esp_ota_begin`,
header write and chunk write failures, `esp_ota_abort` on every failure after
begin, rollback refusals, recovery_exit and boot_guard_reset reporting,
recovery_boot refusals (mode gate, energized or unreadable relay, recovery
image absent, set-boot failure with restore, reboot task creation failure),
Pico push refusals, and registration of exactly 13 distinct routes.

Negative test: 4 mutations (target==running check, `esp_ota_abort` cleanup,
recovery_boot claim release on set failure, Pico relay-start handling), all
CAUGHT.

Not covered here: the `update_*_http*.c` gate-refusal handlers (done in campaign 9c below).

Findings (handler behaviour left as is, per campaign rules; the tests do not
pin either behaviour):

- `ota_recovery_exit_post_handler` sends `{"ok":true,"status":"rebooting"}`
  before it creates the reboot task. If `xTaskCreate` fails, the log says the
  board will not reboot, but the client has already been told it is
  rebooting. It also reports ok when `boot_guard_mark_healthy()` did not
  verify, which is logged only. Severity low: the route is an operator escape
  hatch and the log line names the manual recovery (power cycle).
- `ota_esp_rollback_post_handler` has the same ordering: the "rebooting" body
  goes out before the reboot task creation is checked.
- The recovery_boot branch that releases the claim after an energized-relay
  refusal is shadowed by the mode gate (`relay_authority_heat_run_active` and
  `relays_energized` refuse first), so the post-claim authoritative re-read is
  not reachable through the fakes. Not a defect, a coverage limit.

## Campaign 9c: update_*_http gate refusals

New `test_update_http_refusals.c` (392 checks, 0 failed, including the
`test_update_fetch.c` cases it hooks into). It #includes `test_update_fetch.c`
(`main` renamed, `UF_EXTRA_TESTS` hook before the writer-wedge case), compiles
the real `update_fetch.c`, `update_http.c`, `update_stage.c` and, new, the real
`update_settings_http.c`, over the existing update_fetch fakes. One new fake
knob, `g_fr_alloc_fail_all`.

Covered: settings handler (GET; cfg unmounted and recovery-skipped 503; empty,
oversize and short body 400; firing/autotune 409 shadowing a malformed field;
missing field, over-long repo, embedded NUL, validator reject 400; persist
failure 500, never ok; success), fetch status OOM 500, bad_confirm 400, gate
order (mode gate before interlock before claim), claim_deny body text, check
never takes the claim, stage upload (claim busy body, interlock reason,
autotune, a parked download holding the claim, zero-length and 100 MB
content_len 413, malformed X-Stage-Zones-Cfg/Kilnlink/Uart headers 400
bad_schema_header, downgrade policy 409), stage GET, stage clear and its flash
failure.

Negative test (`tools/negtest.ps1`, run with `-Command` on
`build_host_tests.ps1 -Only update_http_refusals` for speed; baseline passed):
10 mutations, all CAUGHT after one replacement: settings_mode_gate,
settings_cfg_unmounted, settings_persist_fail_as_ok, settings_body_bound,
stage_oversize_status, stage_kilnlink_hdr, stage_claim_busy_body,
stage_clear_mode_gate, download_bad_confirm, fetch_status_oom. The first
attempt, removing the `strlen(repo) != len` embedded-NUL guard in
`update_settings_http.c`, was MISSED because it is shadowed: `http_form_url_decode`
already returns -2 for `%00`, so the guard is unreachable defence in depth.
Replaced by settings_body_bound.

Findings: no defects. Coverage limits, not defects: the mode-gate re-check
after the claim is taken (stage upload) cannot be driven through the fakes;
auth tier is a route-table property, not a handler one, and there is no
recovery-mode gate in the update handlers; there is no apply route among the
`update_*` handlers (apply lives in the recovery image's own handler set).
