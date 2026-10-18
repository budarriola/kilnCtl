# Review 2026-10-18: KilnFW internal-RAM allocation failure and fragmentation (HIGH 0, MED 0, LOW 2, INFO 4)

Scope: origin/dev KilnFW source and `sdkconfig.defaults`, review only, no bench. Owner floor: heap_internal
min_free >= 8192 B. Compared against REVIEW_RAM_WORSTCASE_2026_10_17 (RW-01..RW-11, RW-01/02 fixed) and
REVIEW_RAM_OTA_2026_10_17; only new findings are listed.

Swept: every plain `malloc/calloc/strdup/realloc` and `heap_caps_malloc(...INTERNAL...)` outside tests, every
runtime `xTaskCreate*`, the HTTP JSON/body buffers, the TLS config (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`,
`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`), the Wi-Fi scan path and the LVGL allocator. Plain `malloc()` below
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192` lands in internal RAM.

## LOW

### RF-01 LOW: LCD worker tasks take internal stacks per tap, outside the RW-02 admission
`drivers/ui/ui_lcd_keypad.c:278` (`lcd_pin_ui`, 6144 B), `drivers/ui/ui_page_network.c:273` (`wifi_mode_ui`),
`:668` (`wifi_ap_id_ui`), `drivers/ui/ui_page_network_manage.c:406,459,528` (`wifi_connect_ui`,
`wifi_forget_ui`, `wifi_scan_ui`), 4096 B each. Each is a plain `xTaskCreate()` (internal stack) created on a
tap and deleted when done, each with its own busy flag. The RW-02 fix gates only `http_async_job` and the OTA
interlock on the update-fetch busy probe; these LCD tasks are not gated and not in the RW budget table. Up to
~26.6 KB can be live together beside a ~21.5 KB update check, and the repeated create/delete of 4-6 KB blocks
interleaves with long-lived allocations, which is the pattern that shrinks the largest free block the fetch
needs (6144 B). Failure is handled (busy cleared, "Busy, try again" / "Could not start"), so this is margin and
fragmentation, not fail-open. Fix direction: one long-lived LCD worker with a job queue (or a static stack), or
refuse while the fetch is busy.

### RF-02 LOW: the LCD Wi-Fi mode switch fails silently when its worker cannot start
`drivers/ui/ui_page_network.c:272-278`. When `xTaskCreate(mode_worker_task, ...)` fails (internal heap short or
fragmented), the busy flag is cleared but nothing is logged and no status is shown: the tap does nothing. The
sibling workers set "Could not start" / "Busy, try again". Fix direction: log and show the same status text.

## INFO

- **RF-03** INFO: `http_async_job_try_start()` (`drivers/http/http_async_job.c:102-139`) admits on fetch-busy
  only, not on the largest internal block, before its 10240 B stack. A failed create returns
  RESOURCE_FAILURE and closes the session (handled, fail-closed).
- **RF-04** INFO: the core-dump chunk route takes a plain `malloc()` of up to 4096 B internal per request
  (`drivers/http/diagnostics_http.c:591`); NULL answers 500. Serialized on the httpd task.
- **RF-05** INFO: `panel_spi_draw.c:424` sizes its buffer from `vsnprintf(NULL, 0, ...)` with no cap (internal
  below 8192 B); NULL returns ESP_ERR_NO_MEM. Callers pass short strings.
- **RF-06** INFO: every HTTP JSON response and request body buffer sampled is PSRAM-first with a NULL path and
  a content_len cap; mbedTLS record buffers are dynamic and external; Wi-Fi scan records are `EXT_RAM_BSS`
  (`wifi_prov_api.c:965`); `cfg_fs_mount.c` scan treats NO_MEM as "cannot determine", never as blank. No
  fail-open allocation found.
