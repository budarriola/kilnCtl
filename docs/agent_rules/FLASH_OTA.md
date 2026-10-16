# Flashing, OTA, boot_guard, safety-trip clears

Split out of CLAUDE.md 2026-10-14; history in docs/CLAUDE_HISTORY.md.

## Flashing

- Pico: `debug_program(peer="pico")`. ESP32-S3: **`flash_firmware(confirm=True)`** (exactly
  `True`), OpenOCD only, **never esptool**. `debug_program(peer="esp")` refuses immediately.
- It refuses on a recovery-image or unreadable board state (`allow_unreadable_board_state=True`
  overrides only unreadable). It resolves the `app` partition from `<kiln_fw_root>/partitions.csv`
  and the partition-table offset from `sdkconfig`, flashes `bootloader.bin` + `partition-table.bin`
  + `KilnCtrl.bin`, and refuses an image larger than its partition.
- `kiln_fw_root` override (absolute path of a `firmware/KilnFW`-shaped dir whose `build/` holds
  the three binaries): the sanctioned way to flash a build from a clean worktree at HEAD.
- It records `git status`/HEAD (`KilnFW/flash_provenance.json`) and refuses, naming files, if the
  dirty set touches config-schema/migration/safety code (e.g. `zones_config_*`); override only
  after review: `allow_sensitive_dirty=True`.
- **otadata gap (known, deliberate):** it does not touch `otadata`. A blank/erased `otadata`
  (also left by boot_guard threshold switch and `recovery_enter`, which end in
  `esp_ota_set_boot_partition(factory)`) boots `recovery`, not `app`, until `recovery_exit`
  runs. Post-flash verification (`verify=True` default; polls the running partition and `fw_build`
  vs the .bin) fails loud in that case. `ota_rollback_esp()` does not fix it; rollback is
  unavailable on the single-slot design (update = stage + `recovery_enter` + `recovery_apply_staged`).
  `verify=False` only for bring-up with no HTTP stack.
- After a successful verified flash it clears the boot_guard counter (default on; skipped with a
  note without an admin web session; `reset_boot_guard=False` opts out). Never call
  `boot_guard_reset_counter()` from an unconditional firmware boot path.
- Old-image rollback past a `zones_cfg` schema bump runs on firmware-default PID gains: read back
  `control_get_zones` before heating after any rollback.
- `debug_reset` does not power-cycle I2C peripherals; a trip right after an OTA reboot can be the
  SX1509 failing post-reset init: a second `debug_reset` clears it.
- `get_heap_status` surfaces `reset_reason`/`uptime_s` and an `UNACKNOWLEDGED CRASH REPORT`
  banner; `capability_preflight` refuses a run on a board with an unacknowledged crash.

### Safety trips after a dual reflash

Resetting both processors close together trips S6a (mainFault) while the safety-link handshake
comes up: expected, not a bug (a `POST /api/sw_reset` dual reset does not latch it). Confirm
link-up via `safety_get_status()` and that `trip_reason`/`trip_mask` show only
`SAFETY_TRIP_MAIN_FAULT` before `safety_clear_trip()`. **`trip_mask` is
`1 << (trip_reason - 1)`** (`link_frame_trip_mask_for_reason()`, `firmware/SaftyFW/src/tasks/link_frame.c`);
derive it, do not quote constants (S4/S10 are WARN-only and skip numbers). S6a is reason 6, mask
bit 5 = `0x0020` (not `0x0040`, which is bit 6, reason 7, `SAFETY_TRIP_LINK_DEAD`/S6b).
S6b after the recovery dwell may be cleared without asking when the cause is the recovery image.
Detail: `docs/MCP_SERVERS.md` flash section, `firmware/SaftyFW/docs/ARCHITECTURE.md`.
