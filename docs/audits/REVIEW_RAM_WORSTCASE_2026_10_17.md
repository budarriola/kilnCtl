# Review 2026-10-17: KilnFW worst-case internal RAM

Scope: origin/dev at mint time. Source and `firmware/KilnFW/sdkconfig.defaults` only. No bench access.
Owner floor: heap_internal min_free >= 8192 B. Baseline: a 45-minute bench soak on 2026-10-17 held min_free
104151 B and largest free block 32768 B. The soak did not run an update check, a backup import, an OTA upload
or a socket storm at the same time.

Counts: HIGH 0, MED 2, LOW 4, INFO 5.

## Budget used for the estimates

| Consumer | Internal bytes | Source |
|---|---|---|
| Update fetch (check or download), measured whole-run draw | 21488 (+1024 stager scratch for a download) | `update_fetch_heap.h:158,167`; TLS task stack is PSRAM (`update_fetch.c:1244-1246`); mbedTLS in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`) |
| Fetch writer task stack | 4096 | `update_fetch.c:95,663` |
| http_async_job stack (backup import, crash clear, CT auto-zero) | 10240, one job at a time | `backup_import.c:4667`, `diagnostics_http.c:651`, `safety_cfg_http.c:1893`; the stack must stay internal (`http_async_job.c:120`) |
| httpd task stack | 8192 (permanent) | `wifi_provision_http.c:1270` |
| ESP OTA upload chunk | 4096 static .bss (no heap) | `ota_http_esp.c:108-109` |
| Wi-Fi static RX buffers | 10 x ~1.6 KB = ~16 KB (permanent, in the baseline) | `sdkconfig.defaults:561` |
| Wi-Fi dynamic TX buffers | not pinned; IDF default 32 x up to ~1.6 KB = ~52 KB peak | absent from `sdkconfig.defaults` |
| profile_exec_status_t / firing-history blob / profile_t | 1512 / 1364 / ~428 per call, transient | `update_fetch_heap.h:121`, `profile_executor_status.c:751`, `profile_executor.c:477` |
| kiln_cfg_swap scratch | internal on purpose, small structs | `kiln_cfg_swap.c:1088,1300,1441` |
| LVGL draw buffers | PSRAM only, fails with ESP_ERR_NO_MEM | `lvgl_port.c:1245-1251` |
| flash_xip lock | static recursive mutex, no heap | `flash_xip_lock_esp.c:16,47` |

Largest contiguous internal request found: the 10240 B async-job stack. It fits the 32768 B largest block seen
on the soak. The update fetch also requires a 6144 B largest block (`update_fetch_heap.h:186`).

Worst stack-up the source allows (estimate): baseline 104151 - check fetch 21488 - async job 10240 - async
request copy and job scratch ~3000 = ~69 KB, before Wi-Fi TX. A TX burst bounded only by the IDF default
(~52 KB) would take that to ~17 KB. Above the floor on paper, but the TX term is unmeasured, and nothing checks
the largest block before the 10240 B stack request.

## MED

### RW-01 MED: Wi-Fi dynamic TX buffers are not bounded by the project under concurrent sockets
`firmware/KilnFW/sdkconfig.defaults:549-562` pins the static RX count and the BA window but not
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` (IDF default 32). With 13 httpd sessions
(`wifi_provision_http.c:1020`) pulling dashboards, status JSON and `/app.js` while an update check streams
TLS, the driver can queue up to 32 TX frames of up to ~1.6 KB in internal DMA-capable RAM. lwIP pbufs may sit in
PSRAM under `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, but the frames the MAC sends from are internal. This is the
class `dram_margin.h:10-16` already recorded (2026-08-22: 13 sockets, largest block 8704 B, ERR_CONNECTION_RESET).
Scenario: four browsers on the dashboard plus an update check on a weak link (retransmits hold frames).
Estimate: up to ~52 KB transient. Not measured. Fix direction: measure min_free under a socket storm and pin
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` in `sdkconfig.defaults` to the proven value.

### RW-02 MED: no internal-heap admission for work that runs beside an update check
An update CHECK does not claim the OTA mutex: only the download handler sets `p.claimed = true`
(`update_fetch.c:1356`). `ota_http_check_interlocks()` reads `other_update_in_progress` (`ota_http.c:465`),
not the fetch busy probe, which only the heat path reads (`ota_http.c:574`). So a backup import (10240 B
internal stack, `backup_import.c:4667`), an ESP OTA upload, a crash-report clear or a CT auto-zero job can start
while the check's ~21.5 KB TLS draw is live. `http_async_job_try_start()` (`http_async_job.c:120-128`) and the
OTA upload path have no heap gate. Only the fetch protects itself, by aborting below 9216 B
(`update_fetch.c:879`), and that sample is periodic, so the other job's allocations land first.
Scenario: the user presses "Check for updates", then imports a backup within seconds while the dashboard polls.
Estimate: ~35 KB combined, plus RW-01. Fix direction: hold the OTA mutex (or a heap reservation) for checks
too, or give http_async_job a free/largest-block admission like `heap_admit()`.

## LOW

### RW-03 LOW: LVGL internal fallback is silent after the first log and uncounted
`firmware/KilnFW/App/drivers/ui/lvgl_mem_psram.c:85-97` (and realloc at `:139-143`) falls back to
`MALLOC_CAP_8BIT` (internal) when PSRAM fails, logs once, and keeps no counter, unlike persist_scratch
(FX-05). Scenario: PSRAM fragmentation after long uptime moves widget allocations into internal RAM with no
signal on /api/status. Fix direction: count it like `persist_scratch_note_fallback()`.

### RW-04 LOW: open-coded PSRAM-to-internal fallbacks bypass the persist_scratch counter
`backup_import.c:1317-1320` (`backup_import_slot_scratch_t`), `ui_edit_firing_apply.c:21-23` (profile_t),
`ui_page_edit_firing.c:79-81` (page state). Each retries plain internal with no log and no count. Small
structs with NULL handling, but a degraded PSRAM is invisible on these paths. Fix direction: use
`persist_scratch_alloc()`.

### RW-05 LOW: persist_scratch fallback logs once, then is silent per call
`persist_scratch.h:52-70`. Counted (FX-05) but logged only on the first fallback. A fallback of a struct
larger than 8192 B (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`) goes to plain `malloc()` and can take a large
internal block. Reachable only when PSRAM is exhausted. Fix direction: raise an alarm on a non-zero counter,
not only report the number.

### RW-06 LOW: the system bridge task drops command dispatch for the whole boot on one failed allocation
`firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c:69-76`. A failed internal `uart_proto_message_t`
allocation deletes the task: relay, CT-cal, factory-reset and watchdog-config commands are gone until reboot
(logged once). Boot-time only, when heap is plentiful; listed as a long-lived degradation. Fix direction:
static buffer or retry.

## INFO

- **RW-07** INFO: the login KDF (`web_auth_store.c:281-301`) runs on the httpd task, serialized, with PSA HMAC
  state in mbedTLS external (PSRAM) memory and a 256 B stack buffer (`:422`). No internal heap growth found.
- **RW-08** INFO: the flash_xip lock (`flash_xip_lock_esp.c:16,47`) is a static mutex; the lock paths allocate
  nothing. esp_flash bounce copies for PSRAM sources are IDF-internal and were not sized here.
- **RW-09** INFO: the ESP OTA upload uses a static 4096 B chunk (`ota_http_esp.c:109`) and holds the single
  httpd task, so no other web handler runs beside it; its extra internal draw is lwIP/Wi-Fi RX only.
- **RW-10** INFO: backup export and import bodies, candidates and plans are PSRAM with refusal on failure
  (`backup_export.c:327-336`, `backup_import.c:4026,4142,4362,4413`). The remaining internal use is the 10240 B
  job stack and the small `backup_import_job_ctx_t` (`:4655`).
- **RW-11** INFO: every internal-only `heap_caps_malloc(MALLOC_CAP_INTERNAL)` found has a NULL path
  (`profile_executor.c:477`, `profiles_http.c:330`, `kiln_cfg_swap.c:1088,1300,1441`, `live_profile.c:655`,
  `safety_cfg_http.c:1867`, `firing_stats_cfg_fs.c:167,265`, `update_fetch.c:1141`). A failure at
  `profile_executor.c:477` skips adopting a live edit that tick (logged).
