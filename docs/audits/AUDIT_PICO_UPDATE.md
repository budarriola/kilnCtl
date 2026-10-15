# Audit: RP2040 firmware update (aupicoupd, 2026-10-14)

HIGH 1, MED 1, LOW 4, INFO 4

Scope: SaftyFW `bootloader/` (main.c, metadata.c, persist.c,
recovery_update.c), `src/tasks/update_task.c` and its helpers
(`update_task_reboot_policy.c`, slot linkage, flash guard, metadata write),
`src/update/` (received_ranges, update_receiver), the ESP-side relay
(`KilnFW/App/drivers/net/ota_pico_relay.c`, `drivers/http/ota_http_pico.c`),
pico_auto_update, and the recovery image's `recovery_pico.c`. Code-only, base
origin/dev `d5bbf13fc`.

Not re-reported (see `AUDIT_LINK_UPDATE_LIFECYCLE.md`): LOW-2 (S6a latches
after a relay update) and LOW-3 (the MIN_COMPATIBLE protocol wedge). LOW-1
there (GPIO6 undriven in bootloader recovery) and INFO-4 (pico_auto_update
compiled out, `PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT` = 0) still stand.

Bench note: per ROADMAP row M the bench Pico runs a flat, bootloader-less
image, so HIGH-1 and MED-1 are latent there. They apply to every board that
runs the two-slot bootloader, which is the shipping design.

## Answers to the key questions

- **Power loss or link drop leaves the Pico unbootable or running an
  unverified slot?** An unverified slot is never booted. The bootloader CRCs
  the chosen slot on every boot (`bootloader/main.c:252-267`). A slot becomes
  bootable only at END, after a read-back CRC and a linkage check
  (`update_task.c:1196-1256`, `recovery_update.c:419-457`). A link drop
  mid-transfer self-aborts after the retransmit cap
  (`update_task.c:1334`) and reverts the target slot. It can still end
  unbootable two ways: HIGH-1 (ordinary resets, no fault at all) and MED-1 (a
  power loss during a metadata-sector wrap).
- **ESP and Pico end on incompatible protocol versions with no way out?**
  Only the case already filed as AUDIT_LINK_UPDATE_LIFECYCLE LOW-3. Nothing
  new was found.
- **Heat always off during a Pico update?** Yes. The ESP refuses push and
  rollback through `ota_http_check_interlocks()` (`ota_http_pico.c:396`,
  `:547`). The Pico refuses BEGIN unless the relay is open, no trip is
  pending and the temperature is known and below 100 C
  (`update_task.c:883-885`, `update_receiver.c` preconditions). It refuses
  relay enable while a transfer is active (`safety_core.c:1914`,
  `relay_grace.c:35`). The bootloader holds GPIO6 low (but see the prior
  LOW-1 for recovery mode).

## Findings

### HIGH-1: `boot_attempts` is never cleared on a confirmed boot, so ordinary resets mark both slots BAD

Evidence:
- `bootloader/metadata.c:272-274`: every boot of a bootable active slot
  persists `boot_attempts + 1`. VALID and PENDING_VERIFY are treated the same.
- `bootloader/metadata.c:257-269`: at `boot_attempts >= 3`
  (`BOOTLOADER_MAX_BOOT_ATTEMPTS`), the active slot is marked BAD. The
  bootloader then switches to the other slot if it is bootable, and otherwise
  enters recovery.
- `src/tasks/update_task.c:1425` (`update_task_confirm_tick`) sets only
  `slots[s_own_slot].state = VALID` and persists. `boot_attempts` is left
  as is.
- A grep of the tree finds only three writers that set the field to 0:
  - `update_task.c:1250` (at END, before the reboot);
  - `recovery_update.c:455` (at END);
  - `metadata.c:331` (a rollback).
  No healthy-boot path clears it.
- `firmware/SaftyFW/docs/BOOTLOADER.md:111` specifies "Incremented before
  jumping, cleared on check-in". The check-in clear is not implemented.

Scenario:
1. An update finishes with slot B PENDING_VERIFY and attempts 0.
2. The reboot sets attempts to 1, and the app confirms B as VALID with
   attempts still 1.
3. Three ordinary resets follow (power cycles, a `sw_reset` dual reset, a
   watchdog, an ESP-requested reboot). Attempts goes 2, then 3, and on the
   next boot B is marked BAD.
4. The bootloader falls back to A, the old VALID image, with attempts 1. The
   board is now silently running the previous firmware.
5. Three more resets mark A BAD too.
6. With no bootable slot, the bootloader enters recovery for good
   (`main.c` not-bootable path). The safety processor stays down until a UART
   upload, and the count then starts again.

For a slot installed by `recovery_update.c`, the other slot is usually EMPTY
because the recovery path starts from a default record. Such a board reaches
recovery after only 3 ordinary resets past the confirm boot.

Heat stays off throughout, because GPIO6 is low in the bootloader. The
failure is availability and silent version regression, not safety.

Suggested fix: in `update_task_confirm_tick()`, set `meta.boot_attempts = 0`
when marking VALID. On startup, when the running slot is already VALID and
`boot_attempts != 0`, persist one record with it cleared, after the same
health gate. That costs one metadata write per boot, which the bootloader
already spends.
Alternative: make `bootloader_decide_boot()` apply the cap only to
PENDING_VERIFY, and not increment for VALID. That loses the hang fallback for
a VALID image that later starts hanging, so the app-side clear is preferred.

Host test: `test/test_bootloader_metadata.c` covers `decide_boot` on its own.
No test drives several boots through confirm and checks that a confirmed
slot survives more than 3 resets.

### MED-1: single-sector metadata log; a power loss between the wrap erase and the program leaves no valid record

Evidence:
- `bootloader/persist.c:25-30`: when the 16-record log wraps
  (`bootloader_metadata_next_write_needs_erase`), the whole 4K sector is
  erased and then the one new record is programmed. Nothing preserves the
  previous record across that window.
- The bootloader writes a record on every boot (`metadata.c:243`), so this
  window opens about every 16th boot. Every update writes 2-3 more records
  (STAGED, PENDING_VERIFY, VALID), and so does every revert.
- The app writer has the same shape (`update_task.c:603-609`, `needs_erase`
  through `update_task_metadata_write_verified`). Its read-back catches a bad
  program, but it does not cover a power loss.
- `bootloader/main.c:239-244`: with no valid record, the bootloader calls
  `enter_recovery()` unconditionally, even though both slots hold good, CRC'd
  images.
- `BOOTLOADER.md` ("Two copies in the metadata sector ... a power loss during
  a metadata write always leaves one valid copy") states a guarantee the log
  scheme does not keep at the wrap.

Scenario: a power cut lands during the roughly 45-400 ms sector erase, or
between the erase and the program, on the 16th boot. On the next power-up the
Pico sits in recovery mode with both slots intact. The fix is a UART upload,
and `recovery_update` then starts from a default record (both EMPTY,
`recovery_update.c:140-143`), so the other slot's VALID state is lost too.

Suggested fix:
- Option 1: use two metadata sectors and alternate the erase, so one sector
  always holds the latest valid record (the A/B pattern `config_store`
  already uses).
- Option 2: when `find_latest == NO_SLOT`, have the bootloader try each slot
  whose vector table passes the slot linkage check and whose CRC matches a
  trailer stored in the image, before it enters recovery.
- Option 1 is the smaller change. The layout already reserves 0x10000-0x11000
  for metadata, so it needs a second 4K sector, a layout bump and a
  `format_version` bump.

Host test: none covers an interrupted wrap. `test_bootloader_metadata.c`
tests `next_write_needs_erase` arithmetic only.

### LOW-1: a stalled transfer with every chunk received never times out, and blocks relay enable and reboot

Evidence:
- `update_task.c:1308-1310`: `update_task_periodic_status()` returns early
  once all ranges are complete. It never reaches the retransmit-cap abort at
  `:1334`.
- `update_task.c:1252-1256`: a PENDING_VERIFY persist failure at END returns
  with `s_transfer_active` still true and no revert.
- With `s_transfer_active` true:
  - `safety_core.c:1914` refuses relay enable;
  - `update_task_reboot_policy.c` refuses reboot (`TRANSFER_ACTIVE`).

Scenario: the ESP sends the last DATA chunk, then crashes or is reset before
END. The same happens if the Pico's END persist fails and the ESP's ABORT is
lost. The Pico then refuses to arm until a power cycle, or until a fresh
BEGIN or ABORT arrives. It fails safe (heat off), but the kiln cannot fire,
and a link-level reboot request is refused as well.

Suggested fix: add an inactivity deadline for a complete-but-unended
transfer, reverting through `update_task_abort_internal()`. On an END persist
failure, revert and clear `s_transfer_active`.

Host test: none.

### LOW-2: DATA payload length is not bounded by the header length or the slot end

Evidence:
- `update_task.c:1027-1043` and `recovery_update.c:378-389` accept up to 248
  bytes at any chunk-aligned offset below `total_chunks`.
- Neither clamps `data_len` to `s_header.length - offset`.
- `program_chunk` checks the page count only.

Scenario: with a header length of 851968 (the full 832K slot), the last chunk
sits at offset 851880. A 248-byte payload there programs 160 bytes past the
slot end:
- for target B, into the first config-store sector at 0x1B1000;
- for target A, into the start of slot B (which may be the running image).

The overlap guard only checks the slot range. The final CRC covers only
`length`, so the spill goes unseen. The KilnFW relay sends exact tail lengths
(`ota_pico_relay.c:511`), so this needs a buggy or hostile sender.

Suggested fix: reject DATA where `offset + data_len > s_header.length`.

Host test: none.

### LOW-3: the bootloader metadata write is unverified and returns nothing

Evidence: `bootloader/persist.c:11-32` returns void, with no read-back.
Callers:
- `main.c` (the per-boot attempt increment);
- `recovery_update.c:297`, `:359` and `:457`.

Scenario: a failed program of the PENDING_VERIFY record makes
`recovery_update` report `R_STATE_COMPLETE` (`recovery_update.c:459`) while
the metadata still says STAGED. The ESP recovery UI reports success, and the
next boot either enters recovery again or boots the old slot.

Suggested fix: read the record back through XIP after programming, return
bool, and report FAILED when it does not match.

Host test: none.

### LOW-4: the bootloader CRCs `length` bytes from metadata without a bound

Evidence: `bootloader/main.c:257-261` passes
`slots[chosen].length` straight to `bootloader_crc32` over XIP.
`metadata.c:105` unpacks it without range checking.

Scenario: a record that is CRC-valid but carries a bad length (from a writer
bug, since both writers bound it today) makes the bootloader read past the
slot. A length beyond the XIP window can fault before the watchdog is armed
(`main.c` arms it only at the jump), and the board hangs in the bootloader
until a power cycle.

Suggested fix: treat `length == 0 || length > BOOTLOADER_SLOT_FLASH_SIZE` as
a CRC failure, which routes it into `bootloader_decide_after_crc_fail`.

Host test: none.

### INFO-1: BEGIN erases the target before STAGED is persisted

`update_task.c:987-1005`, `recovery_update.c:355-359`. A power loss in
between leaves erased flash with a non-bootable slot state (or the old state
for a slot that was BAD or EMPTY). The bootloader CRC catches a stale
VALID/PENDING record over erased flash, so this is benign.

### INFO-2: `requested_slot_mismatch` is ignored on the Pico

`update_task.c:956`. A slot-A image
pushed to slot B is caught only by the END linkage check, after the full
transfer. This is correct, just late.

### INFO-3: a second update before rebooting is refused, not mis-targeted

After END, `active_slot` names the new slot, so a second BEGIN targets the
running slot. `update_task_region_overlaps_running_image()`
(`update_task.c:419-430`, `:971`) refuses that. The operator has to reboot
first.

### INFO-4: bootloader recovery hardcodes all preconditions true

`recovery_update.c:310-314`. This is correct only because the bootloader
never drives heat. It depends on AUDIT_LINK_UPDATE_LIFECYCLE LOW-1 (GPIO6
undriven in recovery) being fixed.

## Test gaps

- No multi-boot host test that confirms a slot and then simulates more than
  3 ordinary resets (HIGH-1). `decide_boot` plus `confirm_tick` metadata
  transitions could be driven in `test_bootloader_metadata.c` with a fake
  record store.
- No test of an interrupted metadata wrap (erase done, program missing),
  asserting that the bootloader still finds a bootable slot (MED-1).
- No test that a complete-but-unended transfer, or an END persist failure,
  eventually clears `s_transfer_active` (LOW-1).
- No test rejecting a DATA frame past `s_header.length` (LOW-2).
- `bootloader_persist_metadata` and `bootloader/main.c` have no host
  coverage at all (LOW-3, LOW-4).
