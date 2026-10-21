# Review 2026-10-18: KilnFW OTA failure paths

Scope: the GitHub fetch, the web stage upload and the recovery image, on origin/dev. For each path, power loss or a network drop was traced at these points: mid-download, mid-write, between the otadata switch and the first boot, during boot_guard rollback, and during a Pico image update. Review only; nothing was fixed or built. Rows already in KNOWN_ISSUES (RO, FA, AUP, RC, FX, OF, UP, RECOVERY_IMAGE_REVIEW I1) are not repeated.

Counts: HIGH 0, MED 1, LOW 1, INFO 3.

## Findings

### OP-01 MED: a structurally invalid staged image is marked VERIFIED, and app is erased before it is checked
Files: `App/drivers/update/update_stage.c` (upload_finish), `KilnFW_recovery/main/recovery_apply.c` (step 3), `recovery_image_check.c` (ric_validate_first_chunk).

Before app is erased, the bytes are checked in two ways:
- the stage header SHA-256, which is computed over whatever bytes were received
- `ric_validate_first_chunk`, which checks only the first chunk: magic, chip, segment-0 length, app descriptor and project name

Take a hand-uploaded .bin (web stage path) that is truncated or corrupt past the first chunk, but whose Content-Length matches its own size. It is staged VERIFIED, because nothing outside the upload ties the SHA to a real image. `esp_image_verify` runs only in step 6 (`cb_app_verify`), after app has already been erased and rewritten. The apply then fails with `app_image_verify_failed` and the old app is gone. The board stays in recovery, which boots and keeps heat refused, until someone pushes a good image. Bounded install retries copy the same bad stage again each time.

The GitHub path is less exposed, because the manifest sha256 binds the bytes, but a bad published asset would hit the same thing.

Suggested fix: in recovery, run `esp_image_verify` over the stage region (stage address + STAGE_IMAGE_OFFSET, bounded by image_length) before the first app erase. Optionally run it in the app at stage finish too, before the VERIFIED header is written.

### OP-02 LOW: the rollback reboot task leaks the OTA claim on failure
File: `App/drivers/http/ota_http_esp.c` ota_rollback_reboot_task (715-761). If `esp_ota_mark_app_invalid_rollback_and_reboot` returns, the task deletes itself without calling `ota_http_update_end()`. The client has already been told "rebooting".

This fails safe: the claim stays held, so heat and updates are refused until a manual reboot. It is unreachable on the single-slot table, because `esp_ota_check_rollback_is_possible()` is false and the route answers 409. It becomes reachable if a second app slot is ever added.

### OP-03 INFO: the recovery direct ESP push erases app before esp_ota_end verifies it
File: `recovery_upload.c`. OTA_WITH_SEQUENTIAL_WRITES erases app as the image streams in, and only the first chunk is validated up front. A truncated or corrupt push leaves app invalid. This is inherent to a direct push from recovery: the board is already in recovery, and a retry push is possible.

### OP-04 INFO: when the fetch writer wedges, heat is allowed again while a late flash op may still run
File: `update_fetch.c` run_job/fetch_task. This is deliberate and covered by earlier DEV_FIRMWARE_REVIEW rows. The op writes only to stage, never to app or otadata.

### OP-05 INFO: a timeout on ota_http_update_end leaves the claim held
File: `ota_http.c`. If the 50 ms lock timeout expires, the claim stays held (fail safe: heat and updates stay refused).

## Verified sound
- A stage header torn by power loss decodes as bad_crc and cannot be installed. The header is written last, after the streamed SHA matches a re-read SHA. flush_head erases the old header only after the identity, version and policy checks pass.
- The stage upload handler and the GitHub fetch release the claim on every error path. The GitHub path compares the manifest sha256 before WR_FINISH.
- Install erases otadata before the reboot, so power loss mid-copy boots recovery again.
- set_boot uses the new state, so a crashing new app rolls back to recovery (APP_ROLLBACK). Power loss between set_boot and post_boot leaves a stale request, which the app drops at boot (UP-02).
- Heat is refused for the whole claim (heat_interlock: ota claim plus fetch_busy). Recovery sends only inert keepalive frames.
- The Pico relay (`ota_pico_relay.c`, `ota_http_pico.c`) clears update_in_progress and releases the claim on every path, and sends UPDATE_ABORT on failure.
- `ota_recovery_boot_run` holds the claim across the reboot; its undo and restore paths are fine.
