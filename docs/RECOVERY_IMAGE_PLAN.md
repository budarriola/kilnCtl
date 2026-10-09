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
- **Recovery is UNAUTHENTICATED (owner decision 2026-10-02, supersedes the earlier "keeps its
  own AP-password HMAC auth" decision):** "No password, no key. The esp should present its own
  access point only, showing a random password on the LCD for the wifi access point." Physical
  sight of the LCD is the only access control. See "Unauthenticated recovery" below.

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
- Browser page (self-contained; the browser still computes the Pico CRC32).
- Wi-Fi credential reset (Wi-Fi scope only).
- New routes need the recovery server's handler cap checked.

**W4 Pico relay.** Landed 2026-10-02, host-tested and target-built only; never run on a
board (bench risks in W5). Code: `main/recovery_pico.c` (relay task),
`main/recovery_pico_proto.c` (pure, host-tested by `check_recovery_pico_proto.ps1`),
`components/kilnlink` (links CommonFW framing), routes `/api/recovery/pico/{upload,status,abort}`
. Deviations and facts: the whole image is buffered
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
operator to power-cycle (still no CLEAR_TRIP). SaftyFW follow-up (not done here, the bootloader is untouched):
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

**W5 bench verification.** Done 2026-10-03 (`docs/BENCH_TEST_LOG.md`): recovery flashed with
`flash_recovery`, forced with `recovery_enter`, LCD page, AP join with the LCD passphrase, status
route, relays verified off, `recovery_exit`. ESP upload through the recovery AP also verified
2026-10-03 (`docs/BENCH_TEST_LOG.md`, "ESP upload through the recovery image (W5)"). Remaining: the
boot_guard threshold switch (attempted 2026-10-03 with JTAG resets, not provoked: the healthy mark clears the counter within ~6 s of boot, see `docs/BENCH_TEST_LOG.md`), Pico upload (blocked, the SaftyFW debug probe is disconnected),
`wifi_reset` (route verified 2026-10-03: clears and restarts with a new passphrase; the board was then exited and re-provisioned over UART the same day, but the credential erase itself was not proven, the saved-network list was not read before re-provisioning, see `docs/BENCH_TEST_LOG.md`), the "wifi_storage_fail" path and a no-PSRAM boot (if the fixture can simulate it). Measure internal heap floor >= 8192 B during an upload on BOTH the
PSRAM and the no-PSRAM boot. Measure the `app` erase time and idle-task starvation, then
decide on `CONFIG_ESP_TASK_WDT_PANIC`.

## Audit fixes (2026-10-02)

**Unauthenticated recovery (landed 2026-10-02; supersedes the auth-fallback-secret and per-client
nonce-ring sections that used to be here).** All HMAC challenge-response code is gone from the
recovery image: the challenge route, nonce ring, `X-*` signature headers, lockout, the
`ap_pass`-derived key, the eFuse fallback secret and the `auth_secret_present`/`auth_fallback`
status fields. `GET /api/recovery/status` reports `auth_mode:"lcd_passphrase"`. The SoftAP:
- AP only, WPA2-PSK; the STA interface is never created. Reason: the routes are open, so only a
  client that joined the LCD-guarded AP may reach them; a station on a home network would expose
  them. `esp_http_server` cannot be bound to one netif, so "no STA" is what makes the AP the only
  path.
- A fresh random 12-character passphrase per boot from a 32-character unambiguous alphabet
  (`main/recovery_passphrase.c`, host-tested by `check_recovery_passphrase.ps1`), from
  `esp_fill_random`. RAM only: never in HTTP, JSON, logs, serial or NVS (the driver's own config
  copy is also kept in RAM via `esp_wifi_set_storage`). A stored `ap_pass` is ignored; the SSID
  still comes from `wifi_nvs` `ap_ssid` (default `kilnctl-recovery`).
- LCD (480x320, no scrolling): SSID and passphrase in a large font, the AP IP 192.168.4.1, the
  Heat/relay status line and the latched hold-fault indication.
- PcTools: `recovery_post_client.py` posts unsigned; no recovery tool reads
  `KILNCTL_AP_PASSWORD`. No tool joins the AP; automation that does should read the passphrase
  from `KILNCTL_RECOVERY_AP_PASSPHRASE`. Removed with the auth: `recovery_auth.c`, the
  nonce-ring and mirror-drift checks. `check_recovery_page_crc.ps1` keeps the page-CRC test and
  asserts the page carries no auth code.
- Residual: nobody on the AP is authenticated; anyone who has seen the screen can flash the board.
  Accepted by the owner.

**Relay-hold watchdog (landed).** `recovery_io_hold_relays_off()` verified the SX1509 hold once
at boot only. A 1 s task (`relay_hold`, priority 3, 3072 B stack) now reads RegDir/RegData back,
re-asserts the hold on any mismatch or unreadable expander, and verifies the write. A mismatch
LATCHES `relay_hold_fault` until reboot even when the repair succeeds. Decision logic is the
pure `main/recovery_hold.c`, host-tested by `check_recovery_hold.ps1`. Stack deviation: the
recovery image has no `stack_margin` API (that lives in KilnFW), so the task reports its own
`uxTaskGetStackHighWaterMark` as `relay_hold_stack_free`; 3072 B measured on the
bench 2026-10-03 at `relay_hold_stack_free` 1952 B (idle; recheck during an upload). The LCD pin writer now takes the
same I/O lock as the task, so the two cannot interleave an SX1509 write. `relay_fault` and
`relays_verified_off` in status (and the LCD "Heat: OFF"/"RELAY CTRL FAULT" line) include the
latched hold fault, so a post-boot loss is never reported as healthy. The hold task never draws;
this image has no LCD task, so the status route calls `recovery_lcd_poll_relay_fault()`, which
redraws only when the fault state changed since the last draw.

**`sw_reset` never writes otadata.** It reports whether `app` verifies (response text and log)
but does not call `esp_ota_set_boot_partition()`: for the factory partition that call erases
otadata, creating the blank-otadata state that makes a later bare JTAG flash of `app` boot
recovery instead; the bootloader already falls back to factory for an invalid app image.

**`GET /api/recovery/status` size fields.** `app_size` (and `max_upload`) is the `app` PARTITION size, not the image; `app_image_size` is the byte length (`esp_image_metadata_t.image_len`) of the image `esp_image_verify()` accepted, `null` when it is not verified (same cache as `app_valid`). PcTools renders it as "app image N bytes of M partition".

**New `GET /api/recovery/status` fields.** `uptime_s`,
`reset_reason` (raw enum), `reset_reason_name`, `app_ota_state`, `coredump_present`,
`otadata_blank` (the last two are `true`/`false`/`null` when unknown), `ap_start_count`,
`ap_stop_count`, `ap_stations`, `ap_connect_total`, `wifi_last_event`, `wifi_last_event_age_s`,
`relay_hold_task`, `relay_hold_fault`, `relay_hold_fault_s` (null until a fault),
`relay_hold_last_ok_s` (null until a verified observation), `relay_hold_mismatches`,
`relay_hold_reassert_fails`, `relay_hold_stack_free`. Host tests added with this batch:
`check_recovery_upload.ps1` (streaming loop and ESP OTA sink with stubbed `esp_ota_*`/httpd:
short body, recv timeouts, an `esp_ota_end` failure leaving the boot target alone) and
`check_recovery_hold.ps1`.

## Open risks

- The Pico bootloader's recovery frame set may differ from the application's frame set;
  confirm against `firmware/SaftyFW/docs/BOOTLOADER.md` before W4.
- A partially working PSRAM chip with an unknown MR2 density asserts at esp_psram.c:219,
  which would put the factory image in a reboot loop (IGNORE_NOTFOUND does not cover it).
- After PSRAM not-found the MSPI stays in low-speed mode (flash about 20 MHz for that boot).
- The recovery image is bench-verified for entry, LCD page, AP join, status and exit only (2026-10-03); the
  upload paths have never run on a board.
- `flash_firmware()` does not write `otadata` (see CLAUDE.md): a blank `otadata` boots
  `recovery`, not `app`, so a from-scratch board lands in recovery after a JTAG flash.
