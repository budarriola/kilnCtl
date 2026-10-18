# REVIEW_OTA_APPLY_2026_10_18 (HIGH 0, MED 0, LOW 3, INFO 3)

Review only, no board. Base: origin/dev (207134862 era). Scope: ESP apply path after a stage is VERIFIED:
stage header, recovery copy stage -> app, otadata, boot_guard, rollback, power cut at each step, Pico auto-update.

Files: `App/drivers/update/{stage_header,update_stage,update_install_record,update_install_http,update_stale_stage}.c`,
`App/drivers/http/ota_http_recovery.c`, `App/main_network_http.c`, `App/drivers/net/pico_auto_update_boot.c`,
`KilnFW_recovery/main/{recovery_apply,recovery_apply_esp,recovery_install,recovery_http}.c`, `partitions.csv`.

## Power-cut matrix (single `app` ota_0 + `recovery` factory, rollback enabled, bootloader validates images)

| Step | Bootloader sees | Recovery sees | Result |
|---|---|---|---|
| App: request written, recovery not yet selected | app | - | App boots; stale request dropped (main_network_http.c:302) |
| Recovery: hash/stage_verify (before first erase) | factory (recovery) | VERIFIED stage, request | Auto apply retried; attempts bounded |
| Recovery: copy in progress | factory, or ota_0 that fails validation -> factory | VERIFIED stage (not APPLYING), request | Bounded retry; NVS-dead board stays in recovery (OA-01) |
| After app readback + app_verify, before set_boot | factory | as above | Re-apply (idempotent) |
| After set_boot, before post_boot | ota_0 PENDING_VERIFY | - | New app boots; request dropped as stale, no OK record (OA-03) |
| After post_boot, before stage erase | ota_0 PENDING_VERIFY | - | Stale-stage cleanup drops the header by hash proof (OA-05) |
| New app crashes before mark-valid | ota_0 aborted -> factory | last OK relabelled rolled_back (load_last MED-3) | Board in recovery, by design |

No bricking window found: the bootloader always has a valid `recovery` factory image, `app` is never erased before
the stage hash, stage_image_verify and chip check pass, and otadata is written only after app readback hash and full
esp_image_verify. boot_guard is cleared (verified) before the first erase and again before set_boot.

## Findings

- **OA-01** LOW, `stage_header.h` STAGE_STATE_APPLYING / `recovery_apply.c`. APPLYING is defined and the
  VERIFIED->APPLYING transition is allowed, but no code writes it: recovery copies under a VERIFIED header. The only
  "copy started" evidence is the kiln NVS request attempts counter and the app_modified result. With kiln NVS
  unreadable (RECOVERY_NVS_FAIL_KILN) a cut mid-copy leaves a VERIFIED header plus half-written app, no auto apply,
  and nothing on the stage that says app was touched. Fail safe (stays in recovery, manual apply works). Fix: write
  APPLYING before the first erase (and accept APPLYING as re-appliable), or delete the dead state and its transition.
- **OA-02** LOW (latent, compiled off), `pico_auto_update_boot.c`. The automatic Pico update does not wait for the
  ESP image to be confirmed (mark-valid / boot_guard soak). It is off today (PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT
  0). Once on, a freshly applied PENDING_VERIFY image could push a new Pico pair and then roll back to recovery,
  leaving the Pico on firmware the running ESP side did not ship with. Fix before enabling: gate the task on
  `main_ota_read_valid` (VALID) or boot_guard_boot_complete plus soak.
- **OA-03** LOW, `update_install_record.c` post_boot / `update_install_http.c` drop_stale. prepare erases the last
  result; a power cut (or req_clear failure, which cb_set_boot logs but ignores) between set_boot and post_boot boots
  the new image with the request still stored. The app drops it as stale and records nothing, so
  /api/update/status shows last_install "none" after a good install. Cosmetic, no safety effect. Fix: when the app
  drops a stale request whose sha256/length match the running partition, record OK (else FAIL outcome_unknown).
- **OA-04** INFO. Verify-to-copy TOCTOU: the stage is read three times (hash, stage_image_verify, copy). No writer
  exists in recovery during apply: s_running is a CAS claim shared with the direct upload (recovery_app_write_claim),
  the stage-clear, manual-apply and Pico routes check it on the single httpd task, and auto apply starts before the
  server. A divergence (flash read fault) is caught by the app readback hash after erase (bounded retry). Accepted.
- **OA-05** INFO. A failed stage header erase after a good apply leaves a VERIFIED header for the running image;
  update_stale_stage drops it only after hashing the running partition. No stale reuse. Accepted.
- **OA-06** INFO. ota_recovery_boot_run holds the ESP OTA claim from the interlock through restart and releases it on
  every refusal path (relay, prepare, select). Recovery apply_task clears s_running on failure, never on success
  (restart follows). No claim leak found. Accepted.
