# Internal RAM drift audit, 2026-10-13 (IRAMGATE)

Owner complaint: internal RAM keeps growing though the board sits at the floor
(heap_internal min_free >= 8192 B). Static work only; no board access, no
measurement of heap. Bench numbers come later from the board owner.

## Premise check

Much was already PSRAM-first before this pass: `persist_scratch_alloc()` (persist
layer, backup import, cfgfs status), LVGL objects (`lvgl_mem_psram.c`), web-auth
tables, touch groups, wifi scan buffers, `s_at`, `adaptive_tune_zones[]`,
`firing_shadow`. The real remaining leaks were: (1) ten HTTP/safety drivers using
plain `malloc` (<= 8 KB, so forced internal by `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`),
(2) one 17 KB internal static (`kiln_cfg_store.c` `s_store`), (3) every
`xTaskCreate` stack (always internal), (4) no gate: nothing stopped the next plain
malloc.

## Changes in this pass

Moved to PSRAM (PSRAM-first with internal fallback, via `persist_scratch_alloc`):
`autotune_engine_guard.c`, `firing_compare.c`, `dashboard_autotune_http.c`,
`dashboard_exec_http.c`, `http_auth_http.c`, `readiness_http.c` (5120 B JSON),
`setup_progress_http.c`, `web_auth_login_http.c`, `zones_http.c`,
`safety_cfg_store.c`. All ten are now covered by
`tools/check_persist_scratch_malloc_caps.ps1`. These are per-request or per-call
heap buffers (transient), so the saving is peak heap_internal during requests, not
a fixed amount; estimate: up to ~5 KB (readiness) plus several 1-4 KB buffers
that previously coexisted with an 8 KB http_async_job stack.

Static: `kiln_cfg_store.c` `s_store` (kiln_cfg_store_blob_t, 0x42ec = 17132 B) is
now `EXT_RAM_BSS_ATTR`. It is only touched by tasks under the store lock and handed
to NVS as a plain buffer (esp_flash copes with external source buffers), never from
an ISR or a cache-disabled path. Other big `b` symbols checked and left: `s_at`,
`adaptive_tune_zones`, `s_profiles_fallback`, `s_routes` are already `.ext_ram.bss`;
`s_lvgl_task_stack` (0x2800) is a task stack (see below); `ctx$0` (0x1998), `json$16` (0x3480 per nm; not triaged) and `s_exec` (0xdfc) stay internal this pass: not verified safe for PSRAM (flash-worker / lock-held paths). Candidate for a later pass if the
bench shows pressure.

## Gates

1. `tools/check_internal_alloc_allowlist.ps1` (cached): any plain
   `malloc/calloc/realloc/xTaskCreate/xTaskCreatePinnedToCore` in non-test
   `firmware/KilnFW/App` .c/.h must be on the in-file allowlist with exact count and
   a reason; a new file, higher count, lower count (stale entry) or missing reason
   fails. `heap_caps_malloc` and `xTaskCreate*WithCaps` do not match.
2. `check_kilnfw_dram_bss_budget` now also grades `.dram0.data` (ceiling 25000 B,
   baseline 24503 B) next to the `.dram0.bss` ceiling (re-pinned after the
   `s_store` move; see the check).

## Inventory of plain allocation sites kept (31 files)

See the allowlist in `tools/check_internal_alloc_allowlist.ps1`; every entry
carries its reason. Summary by class:

- Task stacks that write NVS/flash (cache is disabled during the write, a PSRAM
  stack would fault): wifi_prov and the UI workers that call it, kiln_cfg_swap
  worker, danger_mode, http_async_job, OTA ESP/Pico/recovery tasks, update_fetch
  writer, ota_pico_relay, zones current sweep, profile executor.
- IO/owner/bridge tasks (UART, I2C, SPI drivers, may run while flash ops hold the
  cache off): kiln_io_owner, thermo_owner, uart bridges, screen_idle, backlight_pwm
  (task disabled by default; a move gains nothing).
- Early boot / reboot path: ota_confirm (main_network_http.c), sw_reset body,
  pico_auto_update_boot.
- DMA: `panel_spi_draw.c` text buffer must be internal.
- Transient request/boot buffers that COULD move (follow-up, not done to stay
  conservative this pass; all freed on exit): backup_import (2), diagnostics_http
  coredump read buffer, profiles_edit_http profile_t scratch, kiln_cfg_store v1/v2
  migration scratch (3), cfg_fs_mount scan/format buffers (3, touch flash directly),
  log_store reader ctx.
- Sanctioned helpers: `persist_scratch.h` and `kiln_cfg_store_internal.h` fallback
  paths.

## Growth attribution (git log -S since 2026-09-01)

Task-create sites: the 2026-09 additions were WP8 GitHub fetch (`40f5635b7`,
update_fetch writer task), recovery switching (`e25d8a303`), the safety-config
single-flight guards (`beaab290b`, `6bf98270e`), the flash worker and cfg
dual-write close (`9cc5ed06b`, `af6e12ebd`). PSRAM-first scratch landed in
`a12320775` (persist) and `9310367b3` (cfgfs status), explaining why growth
continued where such conversion had not happened. `s_store` is a long-standing
static that grew with each blob version bump (no single commit; `-S` on the
declaration finds none since 09-01).

## Proposal (NOT applied): CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL

Currently 8192: every plain malloc <= 8 KB is internal. Lowering to 2048-4096
would send most remaining small request buffers to PSRAM automatically with no
per-site edit. Risks: any ISR/cache-disabled/DMA/TLS/lwIP consumer using plain
malloc would get PSRAM (lwIP/TLS use their own allocators and usually request
MALLOC_CAP_INTERNAL explicitly; mbedTLS is configured separately). The earlier
experiment recorded in `sdkconfig.defaults` (16384 -> 4096 together with another
change) was reverted for exactly this unknowable-consumer reason. Recommend: only
after the allowlist above is fully triaged and a bench run (heap_internal low-water
plus a full OTA, backup import, TLS fetch) at 4096; keep `RESERVE_INTERNAL`
32768.
