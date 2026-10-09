# Bench rules

Read `COMMON.md` first. These apply to anything that touches the ESP32-S3 main board, the
RP2040 safety processor, the bench camera, or the MCP servers.

## Access

- Every board interaction goes through the MCP facade: `kiln_help()`, `kiln_find()`,
  `kiln_call()`, `kiln_batch()` (kilnctrl on 8767; kicad on 8766). If you do not see a tool,
  search for it; do not conclude it is missing. `kiln_batch` for two or more operations.
- If the servers are down or stale, run
  `powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\mcp_servers.ps1 status`.
  Only run `restart` if the prompt allows it, alone, and re-check `status` afterwards (it
  can take about three minutes).
- COM14 is the main board's serial port. Serial tools need the hub on 8765; if
  `connect()` reports the hub gone, `get_device_log` may be serving a stale cached buffer.
  Confirm live log flow (uptime advancing) before trusting a capture. Release the port when
  done.

## Flashing and resets

- ESP flash only via `flash_firmware()`. Never esptool, `idf.py flash`, or
  `debug_program(peer="esp")`. It records git provenance and refuses a dirty tree that
  touches schema, migration, or safety code; do not override without reviewing the named
  files. Post-flash verify is automatic; treat a soft WARNING as unverified until you confirm
  the running partition and `fw_build` at the LAN address yourself.
- Pico via `debug_program(peer="pico")` with the flat `SaftyFW.elf` only. No slot or
  bootloader images: the two-slot bootloader install is owner-gated NO-GO.
- Never flash or reset during a firing. Resetting both processors close together trips S6a
  (mainFault) by design; confirm link-up and that `trip_mask` shows only that reason before
  `safety_clear_trip()`. `trip_mask` is `1 << (trip_reason - 1)`.
- `debug_reset` does not power-cycle I2C peripherals; a trip that latches right after a
  reboot can be the SX1509 failing init, cleared by a second reset.

## Read-only means read-only

- Never `POST /api/estop/verify`, never call `touch_log_tap_targets`, never acknowledge a
  crash report, never drive GPIO9 over SWD to fake the E-stop, never lower CT noise-floor
  thresholds, never write zones or profile config unless the prompt says to.
- No heating in bench verification unless the prompt authorizes it. If a profile is running
  when you arrive, stop and report; do not flash.
- `bench_test_run`/`ota_matrix_run` refuse fail-closed if `logs/bench_test/.board_lock`
  already names a live holder (any suite; only `smoke`/`static`/`stack` skip taking the
  lock themselves) — do not remove that file or start a second run to "just
  check"; wait for the named run to finish, or report the refusal (pid/suite/start time) if
  it looks stale and you did not start it yourself. The refusal is now mutual: a mutating
  run also refuses while a live `smoke`/`static`/`stack` run has a marker registered under
  `logs/bench_test/.board_readers/` — do not remove those either.
- Login attempts: one per verification, at least 30 s apart, form-encoded
  (`application/x-www-form-urlencoded`, `Accept-Encoding: identity`). Never loop on
  credentials. Credential handling per `COMMON.md`.

## Health and evidence

- `get_heap_status` carries `reset_reason`, `uptime_s`, and an unacknowledged-crash banner;
  poll it before and after any action and report uptime deltas to prove no reboot.
- A missing endpoint means the board is not running the build that has it, not that the
  feature is unimplemented. Check `fw_build` against the commit you expect.
- Judge LCD colours by numeric pixel sampling
  (`tools/PcTools/scripts/capture_lcd.ps1`, `sample_lcd_region.ps1`) against a bezel
  reference, never by eye or by theme constants.
- Name the port in any shared log you write to.

## Hand-back

What you sent or did (exact requests, counts), what the board returned, uptime before and
after, decisive log lines quoted, and anything you could not observe.
