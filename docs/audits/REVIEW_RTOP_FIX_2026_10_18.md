# REVIEW_RTOP_FIX_2026_10_18

Scope: cb1e85565 + a4bfc2d37 (RT-01..RT-04) and 298a4f4c6 (OP-01, OP-02).
Review only: source read, no builds, no negtest runs, no bench.
Counts: HIGH 0, MED 0, LOW 3, INFO 7. Verdict: both fixes sound; one recovery-hint error, one fence gap, target proof pending.

## RT-01: bootstrap refused while the FS-02 latch is held
- Not latched (normal first boot, healthy board): `web_auth_store_storage_reset_bootstrap_allowed()` returns true,
  so behaviour is unchanged.
- Latched: true only if the web blob loads with the right version and CRC, and the admin record is
  `!configured && must_change`. Two functions write that record. One is `web_auth_store_clear_for_physical_reset()`
  (the gesture, wired only from auth_reset_gesture_wiring.c). The other is `web_auth_store_clear_all_credentials()`,
  which is reachable only through the ADMIN-tier `/api/security` dispatch. While latched, `load_policy` reads
  UNREADABLE, so auth is effectively on, no admin record exists, and bootstrap_needed denies every ADMIN route. Nothing
  outside the gesture can write the marker from the network. No backup/import or UART path writes WEB_AUTH_KEY_WEB.
- Post-gesture: bootstrap sets the admin password, and `set_password` (web_auth_store.c:680) runs
  `storage_reset_resolve()`: policy, then sentinel, then the latch is dropped. The marker is persisted, so a reboot
  between the gesture and the bootstrap still allows it. A failed resolve leaves the board latched and the admin
  configured; the F2-02/AF-01 heal finishes the resolve on the next boot. So no path latches forever, as long as the
  gesture can be reached. Locked LCD sessions return to home, and the home corner zones stay live.

### RX-01 LOW: the 403 text and header advertise an LCD admin PIN recovery that does not exist while latched
`security_backend_web_auth.c` bootstrap 403 ("... or set the admin PIN on the LCD") and `web_auth_store.h`
("or set the admin LCD PIN"). `web_auth_store_set_pin()` is called only from `security_backend_web_auth.c` through the
authenticated web ADMIN dispatch, and no UI path sets a PIN. While latched, no admin can log in. The physical gesture
is the only recovery, so the operator is pointed at a dead end. Fix: drop the LCD PIN clause from the 403 text and the
header. The F2-01 LCD banner "set LCD admin PIN" reads the same way after a recovery, but by then an admin exists.

### RX-02 LOW: the restore fence covers ct_auto_zero only; bench_preset and rate_guard/auto can interleave with a restore
`safety_cfg_http.c` ct_auto_zero comment: "a restore fence, like the other safety writes". No other handler in
safety_cfg_http.c checks `backup_import_restore_in_flight()`. Backup import pushes i_normal_a through
`safety_cfg_write_apply_pairs()` without claiming the `safety_cfg_writer` guard (only the sweep, async job, swap,
reconcile and HTTP sync writers claim it). So bench_preset (an async job that stages 32 params and then commits) or
rate_guard/auto (an HTTP sync writer) can stage and commit Pico params while a restore is doing the same. One writer's
COMMIT_CONFIG can then commit the other writer's half-staged set. The restore's read-back confirm catches its own
pairs, but not the preset's. Fix: add the same fence to both handlers, or have the import claim the writer guard.

### RX-03 INFO: the RT-02/03/04 gates are entry checks
`safety_cfg_mode_gate_refuse()` reads `relay_authority_heat_run_active()` once, at handler entry. ct_auto_zero and
bench_preset then run as async jobs (10-15 s). Whether a profile start is refused during the job is decided by the
pre-existing preconditions and the Pico ARMED refusal, not by this gate. The 409 shape and reason match HR-02.

### RX-04 INFO: a partial gesture still leaves the marker
`clear_for_physical_reset_impl` writes the web blob first. If the LCD blob write or its read-back then fails, the
gesture reports failure, but the marker is already set and bootstrap is allowed. Physical presence has still been
shown, so this is acceptable.

### RX-05 INFO: the a4bfc2d37 test covers the no-marker case
A blob with the admin configured or must_change clear stays 403. This complements the cb1e85565 tests: not latched,
latched with no gesture, latched after the gesture.

## OP-01: esp_image_verify over the staged image
- Offsets and lengths: both callbacks use the absolute flash address `part->address + STAGE_IMAGE_OFFSET`, size =
  the staged length (`st->total`, or the header `len` in recovery), and they also require `meta.image_len <= len`,
  the same rule as `cb_app_verify`. The verify runs after the SHA readback and before the VERIFIED header (app), and
  after the header/SHA check and before the first app erase (recovery). A refusal leaves the stage unstaged and app
  untouched.
- flash_xip bracket: app `io_image_verify` brackets with `flash_xip_reader_enter/exit` and calls only IDF image
  functions inside it, which follows the flash_xip_lock.h rule. Recovery has `CONFIG_SPIRAM=y` but not
  XIP_FROM_PSRAM, so the cache is disabled during SPI1 ops and no bracket is needed. This matches the existing
  `cb_app_verify`.
- Recovery made `stage_verify` mandatory (`ERR_ARG` if NULL).

### RX-06 LOW: no target proof that a good image passes, or of how long it takes
The tests stub `esp_image_verify` (stubs_update_fetch/esp_image_format.h, test_recovery_apply.c). A false refusal on
target would block every manual upload, every GitHub stage and every recovery apply. The verify path is the same one
esp_ota_end uses, so a real image should pass, but that has not been shown. Fix: on the bench, upload a real release
.bin, confirm it reaches VERIFIED, and log how long upload_finish takes. Then do one recovery apply of a staged image.

### RX-07 INFO: watchdog and lock cost
esp_image_verify reads the whole image (about 2 MB) through mmap: a checksum over every segment, plus a SHA if one is
appended. It does not yield, and in the app the flash_xip bracket is held for the whole call. During that time every
SPI1 op on any task (NVS, LittleFS, coredump) blocks. Estimated at well under 1 s, against the 5 s TWDT. Those tasks
wait on a mutex rather than spinning, and IDLE starves only if the httpd or fetch writer task outranks it on that core
for the whole call. Unmeasured; RX-06 covers the measurement.

### RX-08 INFO: the update_stage NULL-skip is not reachable on target
The only target `update_stage_io_t` is built in `update_http_start()` and wires `.image_verify`. update_fetch's
WR_FINISH uses the same `s_stage`. The stale-stage io (`update_stale_io_t`) is a different type. No check pins the
wiring, so a future second constructor could drop it silently.

## OP-02: ota_rollback_reboot_task releases the claim
### RX-09 INFO: the release is correct
The handler keeps the ESP claim until the task runs. It releases the claim itself only if xTaskCreate fails, so the
task releases exactly once, and only on the path where `esp_ota_mark_app_invalid_rollback_and_reboot` fails.
`ota_http_update_end()` is not owner-checked, but no other holder can exist while this claim is held. The flash_xip
bracket is released before the release. The 50 ms lock timeout is OP-05.

### RX-10 INFO: OP tests
test_update_stage checks one call over the full length, and that a refusal leaves the stage unstaged and idle.
test_recovery_apply checks that the verify runs before the first erase and that a refusal means no erase and no write.
test_ota_http checks that the claim is released on the stubbed failure. These map to the intended mutants; no negtest
was run in this review.
