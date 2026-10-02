# Recovery image rework plan

Status: opened 2026-10-02. Pending work only; delete a wave's section when it lands and
update the ROADMAP row in the same commit. Background: `docs/OTA_SINGLE_SLOT_PLAN.md`
(why a separate recovery image exists). Code: `firmware/KilnFW_recovery/`, flashed to the
`recovery` factory partition in `firmware/KilnFW/partitions.csv`.

## Owner decisions (2026-10-02)

- The `recovery` partition size does not change; `partitions.csv` is not touched.
- Scope: static LCD status page; web pages to upload a new ESP image AND a new Pico
  (RP2040) image; hold the kiln off; exit-recovery; show WHY the board is in recovery.
- Uploads stream live. ESP image is written into `app` as it arrives (first-chunk header
  check; the boot partition is set only after `esp_ota_end` verifies). Pico image: the
  browser computes CRC32 and sends it with the upload; recovery keeps the whole image in a
  PSRAM copy while relaying so the Pico's gap-retransmit rounds can be served.
- PSRAM enabled in recovery with buffers in PSRAM, but recovery must still boot if PSRAM is
  absent or fails (`CONFIG_SPIRAM_IGNORE_NOTFOUND`), degrading to small internal buffers.
- A Pico update stall must NOT block exiting recovery.
- WPA2 only: WPA3-SAE, WPA enterprise and IPv6 are off.
- The Pico RUN pin is NOT wired (main board A1 pad 30 net unconnected; no BOOTSEL or 3V3_EN
  route either), so the ESP cannot reset the Pico. After a Pico update the page tells the
  operator to power-cycle; if the Pico app answers, recovery may offer SAFETY_CMD_REBOOT
  0x29 (the Pico refuses it while a relay is energized).
- Recovery checks the Pico image's initial SP (SRAM) and reset vector against the target
  slot and refuses the wrong slot variant ("upload the other slot file").
- A latched trip: recovery only shows the refusal. No CLEAR_TRIP in recovery.
- At least 8 KB internal RAM free at runtime (owner rule, heap_internal min_free >= 8192 B).
- Recovery keeps its own AP-password HMAC auth on its routes (the 2026-09-29 HMAC
  retirement applied to the main app only). `recovery_ota_auth_mirror_drift_check.py` and
  `check_recovery_ota_auth_mirror.ps1` stay.

## Waves

**W1 build trims + PSRAM -- landed.** `sdkconfig.defaults`: size optimization,
WPA3-SAE/OWE/enterprise/IPv6 off, 16 MB flash header, optional PSRAM via
`SPIRAM_USE_CAPS_ALLOC` (malloc and network buffers stay internal; big upload buffers come
from `heap_caps_malloc(MALLOC_CAP_SPIRAM)` and must handle NULL). Assertions enabled, log
level INFO, task-watchdog panic off until W5 measures the erase. Remove this paragraph once
W2 starts.

**W2 LCD fix + relays forced low at boot.**
- The panel is an ST7796, not an ILI9488. DC = SX1509 IO15, RST = IO14 is UNCONFIRMED until
  the bench: `firmware/KilnFW/App/drivers/hw/settings.h` lines 157-161 and
  `firmware/KilnFW/App/drivers/common/uart_task_ids.h` lines 367-368 disagree
  (`firmware/KilnFW/docs/DISPLAY_ST7796_WIRING.md`).
- Font must cover digits and lowercase.
- Page shows: boot_guard count, reset reason, crash reason, IP and SSID. Static, 480x320,
  no scrolling.
- SX1509 relays forced low at boot: relays are IO0..IO3, active high; expander ~RESET is ESP
  GPIO10; pin map at `firmware/KilnFW/App/drivers/common/uart_task_ids.h` lines 355-372.
  Recovery must never leave a relay floating or high.

**W3 HTTP.**
- First-chunk validation of the ESP upload: `esp_image_header_t` magic 0xE9, chip ESP32-S3,
  `esp_app_desc_t` project_name "KilnCtrl"; total size bounded by the `app` partition.
- Routes: `recovery_exit`, status (why in recovery, counters, versions), upload.
- Browser page with a self-contained JS SHA-256/HMAC (no SubtleCrypto on plain http).
- Wi-Fi credential reset (HMAC-gated, Wi-Fi scope only).
- New routes need the recovery server's handler cap checked.

**W4 Pico relay.**
- kilnlink UPDATE_BEGIN/DATA/END/ABORT/STATUS per `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`
  section 4; send SAFETY_CMD_ANNOUNCE_REBOOT first. Send BEGIN to whichever receiver
  answers (Pico app or Pico bootloader; wire-compatible). If nothing answers, the page says
  "Pico not responding -- power-cycle"; the ESP cannot reset the Pico.
- Recovery-with-PSRAM is the exception to UPDATE_PROTOCOL.md section 4's "the ESP cannot
  buffer the image".
- Pace DATA frames (the bootloader has a 32-byte UART FIFO and programs with interrupts off)
  and use generous erase timeouts (the bootloader can be silent up to about 120 s after
  BEGIN).
- Whole image in PSRAM (browser-supplied CRC32 checked); abort the relay on browser
  disconnect; a stalled relay must not block recovery_exit.
- No PSRAM: refuse the Pico upload with a clear message (ESP upload still works with small
  internal buffers).

**W5 bench verification.** Flash the recovery image to the bench board, force recovery,
exercise ESP upload, Pico upload, exit, LCD page, relays low, and a no-PSRAM boot (if the
fixture can simulate it). Measure internal heap floor >= 8192 B during an upload on BOTH the
PSRAM and the no-PSRAM boot. Measure the `app` erase time and idle-task starvation, then
decide on `CONFIG_ESP_TASK_WDT_PANIC`.

## Open risks

- The Pico bootloader's recovery frame set may differ from the application's frame set;
  confirm against `firmware/SaftyFW/docs/BOOTLOADER.md` before W4.
- A partially working PSRAM chip with an unknown MR2 density asserts at esp_psram.c:219,
  which would put the factory image in a reboot loop (IGNORE_NOTFOUND does not cover it).
- After PSRAM not-found the MSPI stays in low-speed mode (flash about 20 MHz for that boot).
- The recovery image has never been bench-verified end to end.
- `flash_firmware()` does not write `otadata` (see CLAUDE.md): a blank `otadata` boots
  `recovery`, not `app`, so a from-scratch board lands in recovery after a JTAG flash.
