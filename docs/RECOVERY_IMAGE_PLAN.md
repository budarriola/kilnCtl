# Recovery image rework plan

Status: opened 2026-10-02. Pending work only; delete a wave's section when it lands and
update the ROADMAP row in the same commit. Background: `docs/OTA_SINGLE_SLOT_PLAN.md`
(why a separate recovery image exists). Code: `firmware/KilnFW_recovery/`, flashed to the
`recovery` factory partition in `firmware/KilnFW/partitions.csv`.

## Owner decisions (2026-10-02)

- The `recovery` partition size does not change; `partitions.csv` is not touched.
- Scope: static LCD status page; web pages to upload a new ESP image AND a new Pico
  (RP2040) image; hold the kiln off; exit-recovery; show WHY the board is in recovery.
- ESP uploads stream live: the image is written into `app` as it arrives (first-chunk header
  check; the boot partition is set only after `esp_ota_end` verifies). **Owner decision
  change (2026-10-02, after the W4 review): the Pico upload does NOT stream live.** The owner
  accepted buffer-then-relay: the browser computes CRC32 and sends it with the upload,
  recovery receives and validates the whole image in PSRAM first, and only then relays it to
  the Pico, serving the Pico's gap-retransmit rounds from that copy.
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

**W4 Pico relay.** Landed 2026-10-02, host-tested and target-built only; never run on a
board (bench risks in W5). Code: `main/recovery_pico.c` (relay task),
`main/recovery_pico_proto.c` (pure, host-tested by `check_recovery_pico_proto.ps1`),
`components/kilnlink` (links CommonFW framing), routes `/api/recovery/pico/{upload,status,abort}`
(contexts `pico-upload`, `pico-abort`). Deviations and facts: the whole image is buffered
and CRC-checked in PSRAM BEFORE the relay starts (202 returned, then status polled), not
streamed live; DATA pace is `RPP_DATA_PACE_MS` (15 ms, bootloader) or `RPP_DATA_PACE_APP_MS` (40 ms, application: 4-deep queue drained per 100 ms wake); a lost COMPLETE after END is reported as outcome unknown (no ABORT); no REBOOT 0x29
offer (the bootloader ignores it; the page says power-cycle); no CLEAR_TRIP; a missing
status poll for 90 s aborts the relay. Target slot: Frame A active slot gives the opposite
slot ("pico-app", read after sending ANNOUNCE_VERSION protocol 16); else the bootloader's
reported slot ("pico-bootloader": the UPDATE_STATUS trailer carries active/target slot,
0xFF = unknown, and the bootloader rejects a wrong-slot image at END with state 8
REJECTED_SLOT_LINKAGE); else the operator's slot choice, reported everywhere as "target
unverified" (only an older bootloader that sends no trailer needs it); else the transfer is
refused. The slot is NEVER assumed: a wrong operator choice leaves the Pico unbootable until SWD. The
file's slot comes from its reset vector and a mismatch is refused before BEGIN.
Review fixes (second commit): the relay sends END itself (both receivers go silent once
every chunk is in, so waiting for a "no gaps" status never ends: `rpp_fin_step` in
`recovery_pico_proto.c`, modelled on `ota_pico_relay.c`); the BEGIN resend waits at least 4
IDLE beacons and 5 s and never restarts during an erase; the pace delay is rounded up to
whole ticks and also applies before END; relay buffers live in PSRAM with an 8 KB
internal-free check after context allocation, task creation and UART install; the status
JSON is built into one PSRAM buffer under the relay lock; a trip-pending refusal tells the
operator to power-cycle (still no CLEAR_TRIP); the MAC covers the query string, so `?crc=` and
`&slot=` are authenticated. SaftyFW follow-up (not done here, the bootloader is untouched):
the bootloader IDLE status should report `active_slot`, which would remove the operator
choice. Original requirements below.
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

## Audit fixes (2026-10-02)

**Auth fallback secret (landed).** A `wifi_nvs` with no `ap_pass`, or one outside 8..63
characters (WPA2 passphrase limits), no longer yields an OPEN AP plus HTTP 500 on every
mutating route. The image derives a fallback secret and uses it both as the SoftAP WPA2
passphrase and in place of `ap_pass` as the HMAC key material:

```
preimage = "kilnctl-recovery-auth-v1|" || BUILD_KEY || "|" || mac[6]
secret   = lowercase hex of the first 8 bytes of SHA-256(preimage)      (16 characters)
key      = HMAC-SHA256(secret, "kilnctl-ota-v1")      (the unchanged auth scheme from there)
```

`mac` is the factory eFuse base MAC (`esp_efuse_mac_get_default`, the same six bytes as the
station MAC), raw bytes, not text. `BUILD_KEY` is `RECOVERY_AUTH_BUILD_KEY` in
`main/recovery_wifi.c` (default `kilnctl-recovery-fallback-key-1`; override with
`-DRECOVERY_AUTH_BUILD_KEY=...`). This is a defence against an OPEN AP, not a secret from
anyone holding the binary and the board's MAC. `GET /api/recovery/status` reports
`auth_secret_present` (stored `ap_pass` usable) and `auth_fallback` (its inverse); the LCD
shows "AUTH: FALLBACK". Pure pieces: `main/recovery_auth.c`, host-tested by
`check_recovery_auth.ps1`.

PcTools mirror (TODO, not done): `recovery_ota_auth_client.py` still takes the AP password
from its caller. A board that reports `auth_fallback: true` needs the client to derive the
secret above from the board's MAC (the PC knows it from the AP BSSID or the status route) and
`BUILD_KEY`. `recovery_ota_auth_mirror_drift_check.py` pins only the header, lockout,
context and query markers of `recovery_authenticate_request()`, none of which changed, so it
does not require this mirror yet.

## Open risks

- The Pico bootloader's recovery frame set may differ from the application's frame set;
  confirm against `firmware/SaftyFW/docs/BOOTLOADER.md` before W4.
- A partially working PSRAM chip with an unknown MR2 density asserts at esp_psram.c:219,
  which would put the factory image in a reboot loop (IGNORE_NOTFOUND does not cover it).
- After PSRAM not-found the MSPI stays in low-speed mode (flash about 20 MHz for that boot).
- The recovery image has never been bench-verified end to end.
- `flash_firmware()` does not write `otadata` (see CLAUDE.md): a blank `otadata` boots
  `recovery`, not `app`, so a from-scratch board lands in recovery after a JTAG flash.
