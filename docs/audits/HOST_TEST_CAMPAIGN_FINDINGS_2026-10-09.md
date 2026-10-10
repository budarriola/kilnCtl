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

### F5-1 (LOW) fw_version worst-case length comment miscounts

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

### F6-1 (LOW) re-entering danger mode clears heat_requested without releasing the wire request

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
