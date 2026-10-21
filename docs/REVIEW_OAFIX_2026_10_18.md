# REVIEW_OAFIX_2026_10_18

Read-only adversarial review of the OTA fixes on origin/main 179cb0058: OA-01 (stage APPLYING removed),
OA-02 (Pico auto-update waits for a VALID ESP image), OA-03 (stale install request recorded by hashing
the running partition), RY-01..03 and RX-11 (`stage_image_verify.c`, Kconfig
`KILNCTL_STAGE_CHIP_REV_CHECK`). No board access, no target builds.

Counts: HIGH 0, MED 1, LOW 2, INFO 4.

## OF-01 MED: OA-03 overwrites recovery's specific FAIL result with "outcome unknown"
`update_install_drop_stale_recorded()` (`update_install_record.c`) always writes a result before it
clears a leftover request. Recovery's non-dropping refusals (`refuse()`/`refuse_ex()`:
APPLY_FAILED with detail, COUNTER_FAILED, NVS_UNAVAILABLE) write a FAIL result and keep the request.
For APPLY_FAILED with the app untouched (or attempts exhausted) and the app still verifying,
`update_install_after_fail()` returns EXIT_APP, so the old app boots. Its ota_confirm task then runs
`update_install_http_drop_stale_request()`: the running app does not hash to the request, and the stored
result becomes FAIL / OUTCOME_UNKNOWN with detail 0 and app_modified false. The specific reason is lost.
This is the normal path for the new RY-01 refusal (stage_verify BAD_CHIP_REV, app untouched) and for
any OP-01 stage verify refusal. "Outcome unknown" also tells the user the install may have happened.
Fix: before writing, read the stored result. If it is FAIL for the same id, keep it and only clear the
request. Write OUTCOME_UNKNOWN only when no result for that id exists. Host test: stored APPLY_FAILED
result + stale request + non-matching running app => result unchanged, request cleared.

## OF-02 LOW: chip-rev Kconfig can differ between app and recovery
`KILNCTL_STAGE_CHIP_REV_CHECK` is defined separately in `KilnFW/App/drivers/Kconfig` and
`KilnFW_recovery/main/Kconfig.projbuild`. Nothing checks that both are set the same. App off and
recovery on: the app marks the stage VERIFIED, recovery refuses it at apply (APPLY_FAILED, app untouched,
then OF-01 relabels it). App on and recovery off: the app refuses first, so no harm. Suggest a check that
compares the two sdkconfig values, or a single shared Kconfig file.

## OF-03 LOW: stage verify hashes the whole image with no yield
`stage_image_verify()` `feed()` reads in 512 B chunks with no yield callback. A ~2 MB image is
about 4000 flash reads plus SHA work in one tight loop, on the recovery apply task and the app
upload_finish path. Estimated well under the 5 s task watchdog (about 0.1-0.5 s), but the recovery
copy loop calls `cb_yield` every chunk and this loop does not. Suggest an optional `yield` member in
`stage_image_io_t`, called every N chunks.

## INFO
- OF-04: OA-02 wait. Mark-valid depends on nvs_ok, web_ok, ota_ok and the boot_guard soak, not on the
  safety link, so there is no deadlock with a Pico update the link needs. A JTAG-flashed ota_0 image
  (otadata UNDEFINED) becomes VALID through `esp_ota_mark_app_valid_cancel_rollback`. On a boot where
  httpd never came up, the image stays PENDING_VERIFY and the Pico update is skipped for that boot.
  That is consistent with the image rolling back. The task is still compiled off
  (`PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT 0`), and `is_blocking` defaults to false, so readiness
  is not held. Accepted.
- OF-05: OA-01. App and recovery build the same `stage_header.c`, so they agree that state 3 is
  BAD_STATE. No writer emits 3. Accepted.
- OF-06: RX-11 walk. It matches IDF image_load for a non-secure-boot app: the mapped-segment
  alignment is image-relative (correct for a 64 KB aligned app partition), the checksum is padded to 16 B,
  the hash covers header through padding, every length is checked against `avail` without
  underflow (avail >= 72 is enforced first), and the hash is released on every failure path. Static
  PSA contexts are serialized by the stage claim (app) or the single apply task (recovery). Accepted.
- OF-07: OA-03 hashing runs under the ESP update claim with the shared chunk buffer and yields per
  chunk. A failed claim leaves the request in place (pre-existing LOW-4 behaviour). Accepted.
