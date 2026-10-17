# Review: RAM and OTA on main 81acb5dd7 (2026-10-17)

Read-only review of device code. Scope: flash_xip_lock (844f04bff) and its bracketed mmap sites, PSRAM stacks and
flash_hop (c9d4f2e3d), persist_scratch_alloc fallbacks, OTA upload/stage/install, boot_guard/otadata, recovery
install order (8c87343f5), internal RAM floor. Items already in docs/KNOWN_ISSUES.md (MMAP LOW-3, PM-01..06,
RAM-01..04) are not repeated.

Counts: HIGH 0, MED 1, LOW 1, INFO 3.

## MED

### RO-01: recovery HTTP-failure restart cuts a running unattended install (regression from 8c87343f5)
- `firmware/KilnFW_recovery/main/recovery_main.c:98` (recovery_install_boot now runs before the HTTP loop),
  `:117-123` (restart), `recovery_health.c:63` (`recovery_health_restart_for_http` calls esp_restart unconditionally).
- Scenario: an install request is pending. recovery_install_boot starts the apply task (erases and copies into `app`)
  and returns. recovery_http_start then fails RHEALTH_HTTP_START_ATTEMPTS times (for example under heap pressure, which
  the concurrent apply task now adds to). The image calls esp_restart mid-copy. Before 8c87343f5 the apply started only
  after the HTTP loop, so this could not happen. Each cut attempt leaves `app` partially written and spends one of
  UPD_INST_MAX_ATTEMPTS. Up to RHEALTH_HTTP_MAX_RESTARTS (3) cuts can exhaust the attempts, so the board stays in
  recovery with no bootable app. The Wi-Fi restart path (`recovery_wifi.c:75`) already waits up to 10 min for
  `recovery_apply_busy()`; the HTTP path does not.
- Fix: in the `!http_ok` branch, wait for `recovery_apply_busy()` (same bounded wait as recovery_wifi.c) before
  `recovery_health_restart_for_http()`, or skip the restart while an apply runs (the apply restarts the chip itself on
  success). Add a source-order or policy host test.

## LOW

### RO-02: flash_xip_lock held across esp_restart on the rollback path
- `firmware/KilnFW/App/drivers/http/ota_http_esp.c:739-741`.
- The reader bracket wraps `esp_ota_mark_app_invalid_rollback_and_reboot()`, which never returns on success, so the
  recursive app mutex is held through esp_restart. esp_restart runs IDF shutdown handlers (esp_wifi_stop) and stalls
  the other core. Any handler that waits on another task issuing an SPI1 op (Wi-Fi/PHY NVS) would block forever on the
  mutex, against the rule in flash_xip_lock.h ("never wait on another app task inside a bracket"). The reboot then
  hangs until the interrupt/RTC watchdog instead of a clean rollback. Speculative today (no app shutdown handlers; IDF
  Wi-Fi stop is not known to write NVS), but nothing checks it.
- Fix: split the call. Under the bracket set the previous partition as boot target without rebooting, exit the
  bracket, then call esp_restart(). Or document and pin that no shutdown handler writes flash.

## INFO

- RO-03: `recovery_switch.c:102` verifies the recovery image, then `recovery_switch_select_boot()` (`:56`) verifies it
  again, on the boot path with the XIP lock held each time. This doubles the boot-threshold boot time (whole-image SHA)
  and blocks every SPI1 writer twice. Fix: pass the verified result into the select step.
- RO-04: `ota_http_esp.c:265` `esp_ota_begin()` reads otadata (esp_ota_get_state_partition, IDF esp_ota_ops.c:168)
  outside a reader bracket. It is unreachable in the application today (the single-slot refusal at `:179` precedes it),
  and the recovery build does not enable XIP from PSRAM. Bracket it if a two-slot layout ever returns.
- RO-05: internal RAM floor. The latest bench rows (BENCH_TEST_LOG, XIP from PSRAM) show heap_internal min_free about
  100 KB idle and 14.1 KB after a TLS fetch, both above the 8192 B floor. 844f04bff and c9d4f2e3d add no internal
  static or stack beyond the StaticSemaphore_t (about 80 B). No finding.

## Checked and found clean
- flash_xip_lock core: a failed start releases the mutex; end gives after the IDF end; recursion covers readers that
  also write (esp_ota_end, set_boot_partition, mark_valid). Lock order is app mutex then IDF s_spi1 mutex everywhere.
- Panic/coredump: core_dump_flash.c calls esp_flash_app_disable_protect(true), which swaps in the noos os_func, so a
  panic while the mutex is held does not block the dump.
- No ISR or scheduler-suspended caller of esp_flash in app code. Priority inversion: the recursive mutex inherits
  priority; the longest holder is esp_image_verify of the recovery image (boot path and recovery_enter).
- All 8 bracketed sites call only IDF flash/ota/image/coredump functions; none calls flash_hop or waits on an app task.
- The remaining unbracketed flash reads use esp_partition_read/esp_flash_read, which go through os_func start and so
  take the lock (update_http.c, pico_image_source.c, ota_pico_relay.c, cfg_fs_mount.c, esp_core_dump_image_check).
- flash_hop: runs inline on the worker or an internal stack, otherwise blocks on bx_flash_worker; no hop under a
  reader bracket.
- Update stage/status: claim() serialises against the fetch writer; all s_ota_esp_chunk users are httpd handlers.
