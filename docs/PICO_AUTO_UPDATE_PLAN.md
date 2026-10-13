# Pico auto-update - pending work

Pending items only. Design, owner decisions (3-attempt budget, exact-match identity, refuse-to-fire causes, forward config migration) and the closed work plan live in `docs/PICO_AUTO_UPDATE.md`. Section numbers there are unchanged.

Status: the ESP embeds both SaftyFW slot images (`EMBED_FILES` in `firmware/KilnFW/App/drivers/CMakeLists.txt`, `net/pico_image_embedded.c`), `pico_auto_update_boot.c` picks the slot from the Pico's reported active slot (`pico_update_attempts_next_slot()`), and steps 1-6 plus the software items of section 12 are done. The reopened steps 2 and 5 were verified against the landed code on 2026-10-09 and are closed. Everything left sits behind one flag: `PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT` defaults to 0, so the boot path is compiled inert until the bench install below passes.

## A. Bench install of the two-slot bootloader (gate: hardware, human at the board; NO-GO until it passes)

Board idle and cold, no profile running, every flash `confirm=True`, GPIO6 (K4) must stay low throughout. Never `safety_clear_trip()` before `trip_mask` equals `1 << (trip_reason - 1)` (S6a is 0x0020). Inputs from one commit: flat `SaftyFW.elf`, `SaftyFW_slotA/B.elf/.bin`, the `saftyfw_bootloader` ELF (`firmware/SaftyFW/bootloader/`); `pico_image_freshness.py` checks the slot pair.

0. Baseline: `safety_get_fw_version` (commit, dirty, `config_crc`); `safety_get_status` (link up, S1 ARMED, `abs_max_temp_c`); `debug_read_memory` peer pico 0x10010000 (4 words) and 16 words at 0x101B1000 (config); `get_readiness` `pico_update` row. Any surprise: stop.
1. Install as a unit: `debug_program` peer pico with the bootloader ELF, then the slot A ELF. Read back: 0x10000100 nonzero bootloader code, 0x10011000 slot A initial SP (0x2xxxxxxx), 0x10010000 is 0xFFFFFFFF or `KLN1`; config words unchanged. A mismatch gets one re-write, then stop.
2. Metadata seeding: if 0x10010000 reads 0xFFFFFFFF the bootloader enters recovery (link down; ESP S6b after about 120 s is expected). Seed via `ota_update_pico` with `SaftyFW_slotA.bin`, poll `ota_status` to DONE; expect 0x10010000 reads `KLN1` and the Pico reboots. Failure: stop, report the `ota_status` text and words, do not hand-write a record (owner decides on a PC-side seeding tool).
3. Boot check: `safety_get_fw_version` commit equals the slot image identity and `config_crc` equals baseline; `safety_get_status` link up and ARMED, `abs_max_temp_c` equals baseline and the ESP's; active slot A from the metadata record (`bootloader/metadata.h`).
4. Restore path: `debug_program` peer pico with the default flat ELF; read 0x10000100 and 0x10010000, then version and status. Record (a) flat overwrote the bootloader and boots flat, so `debug_program` is a destructive restore of the pre-bootloader state, or (b) board does not boot. The outcome becomes an owner decision on the sanctioned restore path; re-establish a known state afterwards.
5. Close: `get_readiness` and `safety_get_status`, no heat indication, no new trip. Report regions verified (3), metadata seeded (how), restore outcome (a/b).

## B. Steps 8-11 (gate: A passes, then hardware)

Build with `-DPICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT=1`.
8. Idle cold board, deliberately different expected identity, let the boot path update the Pico unattended. Verify in order: K4 open and no heat for the whole window; relay DONE; Pico reboots reporting the expected identity; `config_crc` unchanged; `abs_max_temp_c` equal both sides; S1 ARMED; attempt counter back to 0.
9. Interrupt a relay mid-transfer. Verify: Pico returns on its old slot still guarding; counter incremented and persisted across the reboot; third failure ends "abandoned" with no further attempts and a non-blocking WARNING on `/readiness` naming the attempt count (budget spent does not refuse firing).
10. Start a firing, reboot the ESP: the updater defers rather than updating, deferral visible on a status surface.
11. Unrecoverable case: force a `NO_IMAGE` or `CHAIN_GAP` outcome; the kiln refuses to start a firing, naming the reason on LCD and `/readiness`; `debug_program(peer="pico")` clears it once identity matches. The carry half needs C.

## C. Step 7: first real `CONFIG_STORE_FORMAT_VERSION` bump (gate: trigger, the next format bump)

Write the v2-to-v3 step in `config_store_unpack_ex()` with a per-step `fields_set` carry-forward and `calibration_missing = !config_params_all_required_set(&rec)`, host-tested in `test_config_store.c` with `i_normal_a` bits set. Wire the chain-gap check (hard `false` today at its one call site) to the image identity's `CONFIG_STORE_FORMAT_VERSION`. The bench board has `i_normal_a` unset on all channels, so the carry cannot be shown on hardware until the owner's CT commissioning sets it.

## D. Protocol-mismatch check (gate: A passes and the flag is on)

Verify `pico_image_embedded_protocol_ok()` on hardware: an embedded image with a mismatched non-zero `link_protocol_version` is refused at boot, `ESP_LOGE` names both versions, and `emb.reason` names the protocol mismatch (not "none embedded"); a matching version proceeds to decide/relay. Also confirm a deliberately stale persisted `last_slot` is corrected from the wire-reported slot. Known limit (not fixable ESP-side): an ESP reboot between UPDATE_END and the Pico reboot pushes the wrong slot and the Pico's overlap guard refuses it.

## E. Measurement owed (gate: any target build)

Re-measure `app` partition headroom with the real embedded pair (8 MiB partition; arithmetic only so far).
