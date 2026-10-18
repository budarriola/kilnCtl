# REVIEW_OTA_RAM_RESERVE_2026_10_18

Review only, base origin/dev 3e6b16064 (after UR-01/UW-01 made `update_fetch_wr` a persistent static task).
Question: which OTA/update paths still take runtime internal-RAM heap that can gate or fail an update, and
which are worth a static reservation under the owner floor (heap_internal min_free >= 8192 B).

Counts: HIGH 0, MED 1, LOW 3, INFO 3. KNOWN_ISSUES prefix: OR.

## Path inventory

| Path | Allocation | Size | Where | Must be internal? | Failure if alloc fails |
|---|---|---|---|---|---|
| Browser stage upload (`update_http.c` upload / upload_finish) | `s_ota_esp_chunk` via `buf_get()`; PSA hash op `s_sha` static | 4096 B | static .bss (internal) | yes, flash write with cache off | none (no runtime alloc) |
| Stage verify (`stage_image_verify.c`, stale-stage work) | same chunk buffer; `stale_work_t` on stack; esp_image_verify on httpd_worker | 4 KB + stack | static / httpd stack | yes (mmap/flash read) | none |
| Install / apply (`update_install_http.c`) | body `char[INSTALL_BODY_MAX+1]` on httpd stack; running-app hash uses chunk buffer + stack PSA op | small | stack / static | n/a | none |
| Browser ESP OTA (`ota_http_esp.c`) | `s_ota_esp_chunk`, `sha_op` on stack | 4096 B | static | yes | none |
| ESP rollback (`ota_http_esp.c:848`) | `ota_rollback_reboot` xTaskCreate | 4096 B stack + TCB | internal heap | no (only waits and restarts) | 500, no reboot (handled, OP-02) |
| Pico browser OTA (`ota_http_pico.c`) | shares ESP chunk buffer | 4096 B | static | yes | none |
| Pico rollback (`ota_http_pico.c:626`) | `ota_pico_rollback` xTaskCreate | 4096 B + TCB | internal heap | stack touches link/UART only | 500, claim released |
| Pico auto-update (`pico_auto_update_boot.c:649`) | xTaskCreate at boot | 8192 B + TCB | internal heap | likely (reads staged image from flash) | ESP_ERR_NO_MEM, warning log, no auto-update this boot |
| GitHub fetch (`update_fetch.c`) | writer static 5120 B (UR-01); fetch task stack PSRAM (WithCaps); work/body/status bufs PSRAM; esp_http_client handle + 2 KB rx/tx bufs, esp-tls ctx, lwIP sockets | ~3-6 KB | internal (each under SPIRAM_MALLOC_ALWAYSINTERNAL=8192) | not inherently, but IDF chooses | refused by FETCH_CHECK_BLOCK_MIN 3072 / free gates, or abort below FETCH_HEAP_ABORT_BELOW |
| TLS (mbedTLS) | record buffers, handshake | ~16 KB+ dynamic | PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, `DYNAMIC_BUFFER=y`) | no | PSRAM exhaustion only |
| httpd async jobs (RF-03 gate) | 10240 B task stacks | 10 KB | internal heap | n/a | BUSY refusal |
| Recovery image upload (`recovery_upload.c`) | chunk PSRAM-first, internal fallback with floor | 4096 B | PSRAM | no | NULL, upload refused |
| Recovery apply/recover/poll/relay tasks | xTaskCreate 8192/8192/poll/6144 | ~22 KB | internal heap | apply/recover yes (flash) | task start error reported |

## Findings

- **OR-01 MED.** The GitHub fetch is the only update path still gated by internal heap at runtime. After UR-01 the
  remaining internal draws are IDF-owned: the esp_http_client handle and its rx/tx buffers, the esp-tls context
  and lwIP socket/pcb objects. Each is under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`, so it lands internal;
  mbedTLS itself is PSRAM. This is why `FETCH_CHECK_BLOCK_MIN` (3072) and `FETCH_HEAP_ABORT_BELOW` stay. They
  cannot be reserved statically from app code (IDF picks the caps). Options, in cost order: (a) keep the gates
  (they refuse cleanly, no partial write); (b) set `buffer_size`/`buffer_size_tx` smaller to shrink the block
  need; (c) lowering ALWAYSINTERNAL globally is not recommended (moves Wi-Fi/LVGL small objects too).
- **OR-02 LOW.** `pico_auto_update` takes an 8192 B internal heap stack at boot. Boot heap is ample, so failure is
  unlikely, and it is logged, not silent. A static stack costs 8 KB of permanent DRAM, which the floor cannot
  spare now (~6.5 KB reserved by UR-01). Not worth it unless the bench shows a boot-time NO_MEM.
- **OR-03 LOW.** The rollback handlers (`ota_rollback_reboot`, `ota_pico_rollback`) create 4096 B tasks on demand
  and answer 500 on failure. `ota_rollback_reboot` only delays then restarts: an `esp_timer` one-shot would remove
  the alloc at no DRAM cost. `ota_pico_rollback` could be static (4 KB DRAM) but rollback is rare.
- **OR-04 LOW.** Recovery image tasks (apply 8192, recover 8192, relay 6144, poll) are dynamic internal. The
  recovery image runs almost nothing else, so fragmentation risk is low; the upload chunk is already PSRAM-first.
  Static only if a recovery bench run shows a start failure.
- **OR-05 INFO.** Browser stage upload, stage verify, install, and browser ESP/Pico OTA use no runtime heap: one
  static 4 KB chunk (`s_ota_esp_chunk`), static or stack PSA hash ops, httpd_worker stack. Nothing to reserve.
- **OR-06 INFO.** The RF-03 largest-block gate in `http_async_job_try_start` is not on any update path (only
  backup import, crash clear, CT auto-zero, bench preset).
- **OR-07 INFO.** TLS buffers are already PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`, `DYNAMIC_BUFFER`,
  `DYNAMIC_FREE_CA_CERT`); no change needed.

## Ranked static-reservation candidates (value / DRAM cost)

1. `ota_rollback_reboot` -> `esp_timer` one-shot: 0 B DRAM, removes one failure path (OR-03).
2. Shrink esp_http_client `buffer_size`/`buffer_size_tx` for API checks: negative DRAM cost, lowers the remaining
   fetch block gate (OR-01). Needs a bench re-measure.
3. `ota_pico_rollback` static stack: 4 KB DRAM for a rare path; only if the floor allows after re-measure (OR-03).

Not recommended now: static `pico_auto_update` (8 KB, OR-02) and recovery task stacks (OR-04).
