# Single application slot plus a recovery image -- pending items

Pending work only. Design, failure matrix and per-step history: `docs/OTA_SINGLE_SLOT.md`.
Verified against origin/dev 2026-10-09.

## Done (removed from the pending list)

- Steps 0-3: recovery project (`firmware/KilnFW_recovery/`), `partitions.csv` with `app`/`recovery`, tooling retargeted (`5e07eeca`; `flash_firmware()` resolves `app` from the CSV).
- Step 4 (migration flash): board runs the current table (`app` 0x210000/0x400000, `stage` 0x610000/0x400000, `recovery` 0xA10000), bench-verified 2026-10-04 (`12d193aa`, `b78e8701`); stage/apply landed on top (`recovery_apply_staged`).
- Step 5 (exercise recovery): `recovery_enter` (`e25d8a30`) then `recovery_push_esp_image` over the SoftAP returned the board to `app` with no cable, 2026-10-03 (`docs/BENCH_TEST_LOG.md`, W5 entries). The "Pico link-dead with K4 open throughout" observation was not separately logged.
- Step 7, threshold half: `recovery_switch_at_boot_threshold()` (`drivers/persist/recovery_switch.c`, called from `main_boot_early.c`) boots `recovery` at the boot_guard threshold.
- Step 8 (docs cutover): `partitions.csv` header, `UPDATE_PROTOCOL.md` (single-slot 409 text), `CLAUDE.md`.

## Pending

1. **Step 6 (bench, human): last-resort USB serial download path.** Erase `otadata` and `app`, write all five artifacts by serial, confirm the board boots. Never run on this board. The standing rule is OpenOCD, never esptool for ordinary flashing, so this needs an explicit owner go-ahead for a one-off.
2. **Step 7 remainder (software, blocked on 6): delete in-app RECOVERY MODE.** Still present: `RECOVERY_MODE_ENABLED` (`boot_guard.h`), `boot_guard_is_recovery_mode()` and its callers (`ota_http_recovery.c`, `ota_http_esp.c`, `readiness_http.c`, `cfg_fs_mount.c`, `dashboard_status_http.c`), the `is_factory_partition` arm of `boot_confirm_decide()`. It is the fallback when the `recovery` partition fails `esp_image_verify()`, so it must not go before step 6 proves the escape path. Verify: three unconfirmed boots land in the recovery image; `boot_guard_reset_counter()` from either image clears it.
