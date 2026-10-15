# Audit: KilnFW_recovery image (aurecimg)

Date: 2026-10-14. Base: origin/dev `153c12cf5`. Static review of
`firmware/KilnFW_recovery/main/` plus the shared `update_install_record.c`. No board access.

The image is unauthenticated by owner decision; that is not counted as a finding. Items already
reported elsewhere and still open (REVIEW_AUTOFX LOW-4: `recovery_install_boot()` runs after
`recovery_http_start()`, `recovery_main.c:128`; GITHUB_UPDATE_CHAIN_REVIEW LOW-3: a first-boot cut
lands in recovery with the stage gone) are not re-reported.

Counts: HIGH 0, MED 1, LOW 2, INFO 8.

Paths below are relative to `firmware/KilnFW_recovery/main/` unless given in full.

## MED

### MED-1: "INSTALL FAILED" hides the SoftAP passphrase and QR for the whole boot; with kiln NVS down it does so on every boot

Locations:

- `recovery_install.c:190-193` (`show_failure()` calls `recovery_lcd_set_error`)
- `recovery_install.c:203-207` (poll task, apply failed), `recovery_install.c:234-242` (refused at boot)
- `recovery_lcd_policy.h:72-77` (`err_head` forces `RLCD_TEXT_ERROR`), `recovery_lcd_policy.h:86-91` (QR hidden in the same mode)
- `recovery_lcd.c:320-323`, `recovery_lcd.c:450-461` (ERROR mode draws the banner in place of the SSID and passphrase lines)
- `recovery_install.c:97-103` (`op_req_read` returns -1 when `RECOVERY_NVS_FAIL_KILN` is set)
- `firmware/KilnFW/App/drivers/update/update_install_record.c:215-217` (a read failure refuses `nvs_unavailable` and does not clear the request)

Scenario:

1. Any unattended install failure: the stage fails a check, the apply fails, or a refusal at boot.
   `show_failure()` sets the error banner. Nothing on this path calls `recovery_lcd_clear_error()`
   (its only caller is the HTTP-start success path, `recovery_main.c:123`, which runs earlier).
2. While `s_err_head` is set, `rlcd_text_mode()` returns `RLCD_TEXT_ERROR`. The SSID, passphrase
   and join QR are replaced by "INSTALL FAILED" and the reason.
3. The passphrase is random per boot and shown only on the LCD (`recovery_wifi.c:267-332`,
   wiped after `esp_wifi_set_config`). The SoftAP and HTTP server are up, but nobody can join.
   The operator cannot reach `/api/recovery/status` to see why, nor `recovery_exit` or a manual
   apply. The only way out is a power cycle.
4. When the `kiln_nvs` partition failed to initialise, `op_req_read()` returns -1 on every boot.
   `update_install_boot_run()` then refuses with `nvs_unavailable` whether or not a request exists,
   and never clears anything. Every recovery boot shows "INSTALL FAILED nvs_unavailable" and hides
   the passphrase, so the board is unreachable over the network on every boot. With USB-serial
   recovery dropped (owner decision 2026-10-09), only JTAG gets it out.

Consequence: a recoverable failure becomes an unjoinable recovery image. This holds for one boot
in the common case and permanently in the kiln-NVS-failure case. The "HTTP FAILED" banner hides
the passphrase deliberately, because the server is down there. For "INSTALL FAILED" the server is
working, and the passphrase is what the operator needs. REVIEW_AUTOFX2 LOW-2 fixed the same defect
for the "DO NOT POWER-CYCLE" banner by switching it to `recovery_lcd_set_warning`. The failure
banner was not switched.

Fix:

- Show install failures with `recovery_lcd_set_warning("INSTALL FAILED <reason>", ...)`, or with a
  third, non-blanking text mode, so the SSID, passphrase and QR stay on screen.
- In `recovery_install_boot()`, when `RECOVERY_NVS_FAIL_KILN` is set, treat the request as unknown
  instead of refused. Show a non-blocking "install state unreadable" warning and do not record a
  FAIL result.
- Pin both with a source test next to `test_recovery_lcd_policy.c`: `show_failure()` must not call
  `recovery_lcd_set_error`.

**Fixed by (recfx):** `show_failure()` now uses `recovery_lcd_set_warning("INSTALL FAILED: <reason>")`, so the SSID,
passphrase and QR stay visible; `op_req_read()` reports "no request" (1) when `RECOVERY_NVS_FAIL_KILN` is set and
`recovery_install_boot()` shows a non-blocking "INSTALL STATE UNREADABLE" warning, so no FAIL is recorded and no
refusal loop. Pinned by `test_install_source` in `test_recovery_lcd_policy.c`.

## LOW

### LOW-1: `wifi_reset` restarts the image under a running Pico relay

Locations:

- `recovery_http.c:823-876` (no `recovery_pico_busy()` check; `restart_soon(500)` at 874)
- Compare with the 409 checks in `recovery_http.c:751` (exit), `recovery_http.c:884` (sw_reset),
  `recovery_http.c:192` (ota_esp) and `recovery_http.c:1101` (apply_staged)
- `recovery_wifi.c:63-87` (`restart_image_for_wifi()` waits only for `recovery_apply_busy()`,
  called at 197 and 215)
- `recovery_pico.c:989` (`recovery_pico_busy`)

Scenario: a `recovery_pico_upload` is relaying an image (BEGIN or DATA in flight). A second client,
or the same one, posts `/api/recovery/wifi_reset`. The ESP restarts 500 ms later. The Pico
bootloader is left in RECEIVING with the target slot erased or partly written. That slot is marked
STAGED, not valid, so the Pico keeps its previous slot. The bootloader has no receive timeout
(SaftyFW `bootloader/recovery_update.c:303-367`), so it waits until the next BEGIN or a power cycle.
The AP-failure restart path has the same gap, but it is milder: the relay aborts once nobody polls
(`recovery_pico.c:348`).

Consequence: the upload is lost and must be retried. A retry works, because a new BEGIN restarts
cleanly. The Pico stays on its old slot, heat stays off (`recovery_io.c:176-330`), and an S6b trip
is expected. Nothing is bricked. Every other restarting route refuses this case with a 409; this
one does not.

Fix: add the same `recovery_pico_busy()` 409 to `wifi_reset_post`. In `restart_image_for_wifi()`,
also wait for `recovery_pico_busy()` (bounded), or abort the relay first, then restart.

### LOW-2: the unattended pre-boot commits OK and clears the request before `set_boot` runs

Locations:

- `recovery_apply_esp.c:129-137` (`cb_set_boot` calls pre_boot, then `esp_ota_set_boot_partition`)
- `recovery_install.c:148-160` (pre_boot hook)
- `firmware/KilnFW/App/drivers/update/update_install_record.c:159-179` (`update_install_pre_boot`:
  clears boot_guard, clears the request, writes the OK result)
- `recovery_install.c:195-207` (poll task records FAIL `apply_failed`; its comment says the
  request stays for a bounded retry)

Scenario A, `set_boot` fails after pre_boot (an otadata write error):

1. The request is already gone and the result says `ok`.
2. The poll task then overwrites the result with FAIL `apply_failed` and shows INSTALL FAILED,
   which also hides the passphrase (MED-1).
3. No retry happens on the next boot, contrary to the comment.
4. `app` holds a fully verified image, so `recovery_exit` would boot it. The LCD and record say
   the opposite.

Scenario B, power is lost between the OK write and the otadata write: the board reboots into
recovery (otadata still points at factory), and status reports `last_install ok` for an install
that is not booted.

Consequence: wrong records in rare fault windows. The device itself stays safe: recovery stays
bootable and `app` is intact in both cases. A first-boot rollback of the new app, which also
leaves `ok` behind, is a related case; `app_ota_state` shows it as aborted.

Fix:

- Have pre_boot write an intermediate result (for example `booting`) and keep the request.
- Clear the request and write `ok` only after `esp_ota_set_boot_partition` returned `ESP_OK`,
  that is, after `cb_set_boot` returns, before the restart.
- Record a distinct `set_boot_failed` reason.
- Correct the comment at `recovery_install.c:195-196`.

**Fixed by (recfx), LOW-1:** `wifi_reset_post` returns 409 when `recovery_pico_busy()`; pinned by `test_http_source`
(`check_recovery_lcd_policy.ps1` mutant `wifi_reset_no_pico_busy`). The `restart_image_for_wifi()` wait is not changed.

**Fixed by (recfx), LOW-2:** `update_install_pre_boot()` now only runs the boot_guard clear; the new
`update_install_post_boot()` (request clear, then OK result) runs from `cb_set_boot` only after
`recovery_boot_partition_set_and_verify()` succeeded (`recovery_apply_esp_start(app, pre_boot, post_boot)`). A failed
switch leaves the request and no OK record, so the poll task's FAIL `apply_failed` and the retry comment hold. Tests:
`test_pre_boot` in `test_update_install_record.c`, `test_apply_esp_source`. No new `set_boot_failed` reason code and no
intermediate `booting` result were added (reason enum is shared with the application/status surface); scenario B now
re-applies from the kept request, bounded by `UPD_INST_MAX_ATTEMPTS`.

## INFO

1. The unattended apply does not invalidate the app verify cache (`recovery_http.c:122-152`). The
   manual path invalidates it (`recovery_http.c:1125`). This matters only in the REVIEW_AUTOFX
   LOW-4 window, and exit is still protected because `esp_ota_set_boot_partition` validates the
   image itself.
2. The unattended apply has no boot_guard pre-clear check before its first erase
   (`recovery_install.c:162-170`). The manual path refuses when the clear cannot work
   (`recovery_http.c:1115-1121`). If pre_boot's boot_guard clear later fails, the new app boots
   with a stale counter and may fall back into recovery. The image itself is intact.
3. The TWDT comment at `sdkconfig.defaults:71-77` says `esp_ota_begin()` erases the whole 4 MiB
   `app` partition in one call. The code uses `OTA_WITH_SEQUENTIAL_WRITES`
   (`recovery_upload.c:180`), and the apply erases 64 KB ahead with yields
   (`recovery_apply.c:125-150`). The comment is stale.
4. `cb_app_verify` runs `esp_image_verify` over the whole app image (about 2.5 MB) with no yield
   (`recovery_apply_esp.c:118-127`). This is probably under the 5 s TWDT, and TWDT panic is off
   (warning only), so it is harmless today. Measure it if TWDT panic is ever enabled.
5. The relay ANNOUNCE says protocol 16, min 7 (`recovery_pico_proto.h:35-36`), while CommonFW is
   at 18 (`firmware/CommonFW/include/kilnlink/kilnlink_version.h:375`). The Pico accepts 16 and downgrades its
   frame formats. The relay uses only GET_STATUS and the BEGIN, DATA, END and ABORT commands,
   which do not change between versions, so this is harmless. Bump it with the next protocol
   change.
6. `update_install_boot_run`'s request buffer is `UPD_INST_REQ_SIZE + 16`
   (`update_install_record.c:209`). A larger request record from a future application reads as a
   failure, refuses `nvs_unavailable` and is never cleared (`update_install_record.c:215-217`),
   which feeds MED-1. This is a forward-compatibility concern.
7. Every `GET /api/recovery/status` scans otadata in up to 128 reads of 64 B
   (`recovery_http.c:440-461`). This is cheap but polled. A per-boot cache would do.
8. The `app_main` stack (4608 B, `sdkconfig.defaults:88`) has no measured high-water mark in this
   image yet. This is the W5 bench item, not new.

## Verified OK

- The passphrase never reaches HTTP, logs or NVS:
  - `esp_wifi_set_storage(WIFI_STORAGE_RAM)` (`recovery_wifi.c:385`).
  - `cfg` and the buffer are wiped on every `start_softap` path (`recovery_wifi.c:267-332`).
  - The log says only that it is on the LCD.
- The Pico relay never sends CLEAR_TRIP or ARM. The relay hold task re-asserts the SX1509 relays
  low every 1 s (`recovery_io.c:176-330`), so heat stays off and an S6b trip is expected.
- `recovery_pico_reserve` refuses:
  - an empty body;
  - anything over the 832 KB slot;
  - the case where PSRAM is missing;
  - the case where the relay is busy (`recovery_pico.c:148-194`).
- Uploads:
  - `recovery_upload.c` checks the length (`:69`).
  - It reads with a deadline and bounded timeouts (`:34-58`).
  - Error replies close the connection (`:150-169`).
- Apply:
  - The copy never touches the stage, which stays VERIFIED.
  - A power loss at any step before `set_boot` leaves otadata on factory, so recovery boots again
    and the apply can rerun.
  - The unattended path raises its attempts counter before any erase (`update_install_record.c:244`).
- Concurrency:
  - The apply start uses a compare-exchange plus an internal-heap floor (`recovery_apply_esp.c:187-226`).
  - There is a single httpd task.
  - The Pico busy flag guards every restarting route except the one in LOW-1.
- boot_guard: clear and verify by key absence (`recovery_http.c:639-727`). Exit refuses on a busy
  relay (`recovery_http.c:751`).
