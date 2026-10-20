# REVIEW_OTA_HEAP_2026_10_20 (OH)

Static review of the KilnFW GitHub self-update / OTA install path on origin/dev (worktree base at review time).
No code changed, no board used. Counts: HIGH 0, MED 1, LOW 3, INFO 2.

## Answers to the three questions

1. **low_heap threshold.** The block gate is `FETCH_CHECK_BLOCK_MIN` 3072 B (`update_fetch_heap.h:207`, applied at
   `update_fetch_heap.c:9`). A 10752 B largest block passes it by 7.7 KB, so on dev it does not cause a refusal and
   should not. pre.3's `low_heap` came from pre.3's own older FREE rule: idle free 29815..30111 B was under pre.3's
   need (UP-03). dev needs 28256 B before the scratch (`FETCH_HEAP_PRECHECK_MIN`; 27232 B held) and idles at about
   45 KB under XIP, so it admits with about 17 KB to spare. Nothing in dev changes pre.3's verdict, so a pre.3 board
   needs JTAG or a recovery push to leave pre.3.
   Internal need of one TLS fetch: the model draw for a whole run is 18016 B (`FETCH_HEAP_MODEL_DRAW_BYTES`). The one
   reading from the current build (xipland) was 15936 B. The mbedTLS records are in PSRAM
   (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`). The internal share is the esp_http_client
   rx/tx buffers (2 x 2048 B, OH-03), the lwIP/Wi-Fi traffic, the socket and esp-tls structs, and concurrent httpd
   load. That comes to about 8-11 KB per handshake (`update_fetch.c:882`).
2. **PSRAM.** Everything the fetch owns is already in PSRAM: the TLS task stack, 12288 B (`update_fetch.c:1296`); the
   `work_t` with its 4096 B read chunk (`:1193`); the 256 KB API body (`:1194`); the job context (`:1524`); and the
   status buffers (`:1455`). Two buffers must stay internal: the static 5120 B stack of the flash writer task (`:107`,
   because that task writes flash) and the 4 KB httpd chunk the routes share (`ota_http_esp.c:110`). The 1024 B
   scratch could move (OH-06). The rest of the internal draw is inside IDF components (OH-03).
3. **Status/install/rollback.** The install path is sound: ADMIN tier, mode gate, interlocks, update claim, relay
   re-read, prepare/undo, verify-then-select and the OTA record. Recovery clears boot_guard before it changes the boot
   target (`recovery_install.c:194,285`). The gap comes after the install (OH-04).

## Findings

- **OH-01** LOW, `docs/KNOWN_ISSUES.md:20` (also `docs/bench/BENCH_UPDATE_2026-10-15.md:26`). Problem: the summary row
  gives "largest internal block 10752 B" as the cause of pre.3's refusal. UP-03 shows the cause was the free check,
  and the dev block gate is 3072 B. A reader would chase fragmentation (RAM-02) when the real limit is the total free
  heap in pre.3 code. Fix: reword the row to "pre.3 free-heap admission refuses (idle about 30 KB, under pre.3's need);
  dev admits; a pre.3 board needs JTAG or a recovery push".
- **OH-02** LOW, `update_fetch_heap.h:14-64,183`, `docs/KNOWN_ISSUES.md:125`. Problem: admission still derives from the
  pre-XIP benchupd4 draw (21488 B less the 4496 B writer share). Only one reading comes from the current build, and it
  predates UR-01. The prose still quotes 31728/32752/30192 B. UP-03 cites `FETCH_LARGEST_BLOCK_MIN` 6144 B, a constant
  that no longer exists (it is now `FETCH_CHECK_BLOCK_MIN` 3072 B). Nothing here is unsafe, but anyone tuning the
  numbers is misled. Fix: take 4 more `heap.min_free` readings on the current build (the BENCH-PENDING note), then
  rewrite the table with the current values and correct the constant name in UP-03.
- **OH-03** LOW, `update_fetch.c:816-817`, `sdkconfig.defaults:359`. Problem: esp_http_client allocates its rx and tx
  buffers (2048 B each) with plain calloc. Under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192` both land in internal RAM.
  These 4 KB of every fetch's internal draw look movable but are not. The header (`update_fetch_heap.h:74`) lists them
  beside the PSRAM record buffers and does not say they are internal. Fix: document them as internal. Optionally cut
  `FETCH_TX_BUF_BYTES` to 1024 B, once the longest request line (the signed GitHub asset redirect URL) is measured and
  esp_http_client is confirmed to handle a URL longer than the tx buffer. That saves 1 KB.
- **OH-04** MED, `firmware/KilnFW_recovery/main/recovery_install.c:170`, `firmware/KilnFW/App/main_network_http.c:297`,
  `sdkconfig.defaults:517`. Problem: recovery records the install result `ok` as soon as `esp_ota_set_boot_partition`
  succeeds. The new image then boots PENDING_VERIFY (app rollback is on) and is confirmed only after
  `BOOT_CONFIRM_SOAK_MS` (30 s). A reset inside that window rolls it back. The reset can be a crash, the 20 s RTC
  watchdog, a power cut, or the BC-01 class (`sw_reset_esp` without `wait_for_confirm`). On the single-slot layout the
  bootloader then falls back to factory, which is recovery. `last_install` in `/api/update/status`
  (`update_install_http.c:299`) still says `state=recorded code=ok`. Only recovery's otadata view (`recovery_http.c:478`,
  "aborted") is accurate, and nothing joins the two. The app never writes "confirmed" back. Fix: on an OTA slot, the
  app marks the result confirmed at `BOOT_CONFIRM_STEP_DONE` (a new flag or detail). Status reports `pending_confirm`
  until then. At boot, recovery rewrites a matching `ok` result to `reason=rolled_back` when
  `esp_ota_get_last_invalid_partition()` (`recovery_install.c:226`) is the installed slot. Host-test both transitions.
- **OH-05** INFO, `update_install_http.c:98-105,299`, `ota_http_esp.c:103-110`. The status GET borrows the shared 4 KB
  `s_ota_esp_chunk` through `app_stage_info` without taking the update claim. This is safe only because httpd runs one
  task and no OTA route goes through `http_async_job`. During a fetch, `update_stage_get_status` returns BUSY and the
  route reports `stage_busy:true, staged:false`, which is correct. Fix: none now. Keep the warning comment, and add a
  static check if async handlers ever reach the OTA routes.
- **OH-06** INFO (RAM-02 interplay), `http_async_job.c:122`, `update_fetch_heap.h:131-141`. The RAM-02 work in progress
  (the async-job 10240 B stacks went back to internal RAM after the PSRAM revert) does not change any fetch gate. The
  dev block gate is 3072 B, so a job holding the largest block cannot cause a fragmentation refusal. The job does lower
  free heap by about 10 KB, but from about 45 KB idle that still clears the 28256 B precheck. A job created during a
  fetch is a step larger than `MAX_STEP` and sits outside the draw model. The expected outcome is a clean
  `low_heap_midrun`/`low_heap_dma` abort or a failed job create. A drop between two samples is the residual risk
  already stated under "Limits". Fix: after RAM-02 lands, run one `update_stage_release` alongside a backup import and
  record `heap.min_free`. Separately, the 1024 B static `s_scratch` (`update_fetch.c:108`) could move to PSRAM, because
  `esp_flash_write` bounces a source outside DRAM through a stack buffer. The gain is small.
