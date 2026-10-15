# Internal RAM audit under XIP (auiram, 2026-10-14)

Static audit only. No code was changed and the bench board was not touched.

- **Build audited:** dev tip `153c12cf5`, KilnFW target build through
  `check_00_kilnfw_target_build.ps1`, with `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` and
  `CONFIG_UART_ISR_IN_IRAM` off (the variant C configuration landed in
  `docs/audits/XIP_PSRAM_INVESTIGATION.md`).
- **Sources:** `KilnCtrl.map`/`KilnCtrl.elf`, `esp_idf_size`, gdb `sizeof()`
  on the ELF, and the firmware source.
- **Owner budget:**
  - `heap_internal` min_free must stay at or above 8192 B;
  - a GitHub fetch needs at least 32752 B of internal free when it starts.
- **Bench baseline for this config:** about 45 KB internal free at idle,
  min_free about 28.8 KB, largest free block 16384 B.

## 1. The rule that changed under XIP

The old rule was "a task with a PSRAM stack must never write NVS or flash".
It no longer holds. Two different flash paths behave differently, and every
placement decision below depends on which one a task can reach.

- **Flash erase and write no longer disable the cache.**
  - `spi_flash_os_func_app.c:32` defines `SPI_FLASH_CACHE_NO_DISABLE`, which
    is true when `SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA`. XIP selects
    both.
  - With it set, `spi1_start` only takes `s_spi1_flash_mutex`. It never calls
    `cache_disable`.
  - So NVS, LittleFS and `esp_partition_write`/`esp_flash_write` now run with
    the cache on, and a PSRAM stack is safe for them.
  - A PSRAM source buffer also works. `esp_flash_api.c:1114-1142` copies a
    non-DRAM buffer through a small stack bounce, which is slower but correct.
- **mmap still freezes the cache.**
  - `esp_mmu_map.c` (map at :445, unmap at :642) calls `s_stop_cache()`. On S3
    that is `esp_cache_freeze_caches_disable_interrupts()`
    (`esp_cache_utils.c:94`).
  - That function asserts `s_task_stack_is_sane_when_cache_frozen()`, so the
    calling task needs an internal stack.
  - These IDF calls reach that path: `esp_image_get_metadata`,
    `esp_image_verify`, `esp_ota_end`, `esp_ota_set_boot_partition`,
    `esp_ota_get_partition_description`, `esp_ota_get_state_partition` (it
    reads otadata through `esp_partition_mmap`), `esp_ota_mark_app_valid_*`,
    `esp_partition_mmap` and `bootloader_mmap`.
  - `esp_core_dump_get_summary` and `esp_core_dump_image_check` should be
    treated as mmap callers until proven otherwise.

**Where the firmware calls into the mmap path:**

| Call site | Call | Task it runs on |
|---|---|---|
| `dashboard_http.c:135` | `esp_image_get_metadata` | app_main at registration, `dashboard_http.c:997` (one-shot, cached; not an httpd blocker) |
| `ota_http_esp.c:351` | `esp_ota_end` | httpd_worker |
| `ota_http_esp.c:383`, `:561` | `esp_ota_get_partition_description` | httpd_worker |
| `boot_partition_verify.c:20` | `esp_ota_set_boot_partition` | caller's task |
| `recovery_switch.c:41` | `esp_image_verify` | caller's task |
| `crash_report.c:582` | `esp_core_dump_get_summary` | bx_flash_worker |
| `main_boot_early.c:240` | `esp_core_dump_image_check` | app_main |
| `ui_page_diagnostics.c:678` | `esp_ota_get_state_partition` | lvgl task, on page refresh |
| `main_network_http.c:780` | ota_confirm task, `esp_ota_mark_app_valid_*` | ota_confirm (transient, internal) |

So a task can move its stack to PSRAM exactly when none of the calls above
can be reached from it.

## 2. Where internal DIRAM goes

Figures are from `esp_idf_size`.

- **DIRAM used 180205 of 341760 B:**
  - `.bss` 80792 B;
  - `.text` (IRAM code placed in DIRAM) 74611 B;
  - `.data` 24802 B.
- **Fixed regions:** IRAM0 cache region 16384 B. External `.bss` 151420 B is
  already in PSRAM.
- **Runtime heap:**
  - About 161 KB is left for the heap after statics.
  - At idle only about 45 KB of that is free. About 116 KB goes to runtime
    consumers: Wi-Fi static RX/TX DMA buffers, task stacks, driver rings, and
    allocations at or below `SPIRAM_MALLOC_ALWAYSINTERNAL` (8192).
  - One byte removed from internal `.bss`/`.data`/IRAM `.text` adds one byte
    of heap.

### 2.1 Largest `.dram0` symbols

| Symbol | Bytes | Where | Must be internal? |
|---|---:|---|---|
| `s_lvgl_task_stack` | 10240 | `lvgl_port.c:1354` | Today yes: the lvgl task reaches `esp_ota_get_state_partition` (`ui_page_diagnostics.c:678`). The NVS-write reason in the comment at `lvgl_port.c:1340-1353` is stale under XIP. |
| `ctx` (`main_boot_ctx_t`) | 6552 | `main.c:246` | Mostly no. `SafetyLinkClass` 4520 B and `uart_protocol_t` 1208 B are task-only data (the UART ISR is not in IRAM). The small SPI/I2C device objects stay. |
| `port_IntStack` | 4192 | FreeRTOS port | Yes (ISR stack, 2 x 2096). |
| `s_ota_pico_chunk` | 4096 | `ota_http_pico.c:65` | No. It is a UART-bound copy buffer. |
| `s_ota_esp_chunk` | 4096 | `ota_http_esp.c:104` | Preferably internal so `esp_ota_write` avoids the bounce, but it is only live during an upload. |
| `s_exec` | 3580 | `profile_executor.c:96` | No. It is touched only by tasks, under `s_exec.lock`. |
| `s_unknown_paths` | 2048 | `pref_cfg_fs.c:297` | No. |
| `s_degraded` | 1920 | `cfg_fs.c:294` | No. |
| `s_coredump_stack` | 1892 | IDF espcoredump | Yes (panic path). |
| `s_bulk_pairs` | 1846 | `safety_cfg_write.c` | No. |
| `json` | 1536 | `safety_stack_margin_http.c:160` | No. Its sibling `safety_cfg_http.c:615` already uses `EXT_RAM_BSS_ATTR`. |
| `fst_storage` | 1512 | firing stats | No. |
| `packet` | 1460 | IDF mdns_send | IDF-owned. |
| `s_wifi`, `s_scan_buf`, scan results (3 x 700) | 4284 | wifi_prov / UI network pages | No. |
| merged `.rodata.*.str1.1` | 6866 | `libspi_flash` `spi_flash_os_func_app.c` input (map line 47207) | Yes for now. This is the merged pool of string literals for noflash/IRAM code, which `esp_idf_size` attributes to the first input. It shrinks only if IRAM placement options are dropped (section 2.3). |

### 2.2 Internal task stacks

| Task | Bytes | Create site | Lifetime | Reaches mmap? | Verdict |
|---|---:|---|---|---|---|
| bx_flash_worker | 10240 | `uart_bridge_ext.c:533` | permanent | yes (crash_report job) | Keep, or split: a small internal worker for mmap jobs and PSRAM for the rest. |
| lvgl (static) | 10240 | `lvgl_port.c:1354` | permanent | yes (`ui_page_diagnostics.c:678`) | Movable once that one call hops to bx_flash_worker or is cached at boot. |
| main task | 10240 | sdkconfig | permanent | yes (boot) | Keep. |
| http_async_job | 10240 | `http_async_job.c:122` (sizes from `backup_import.c:4591`, `diagnostics_http.c:647`, `safety_cfg_http.c:1856/2045`) | transient | no | Movable. Each job needs a contiguous internal 10 KB block (the largest free block is 16 KB). |
| httpd_worker | 8192 | `wifi_provision_http.c:1270` | permanent | yes (`ota_http_esp.c:351/383/561`) | Keep until OTA finalisation hops to an internal worker. After that, `config.task_caps = MALLOC_CAP_SPIRAM` (`esp_http_server.h:176`). |
| kiln_cfg_swap | 8192 | `kiln_cfg_swap_worker.c:218` | permanent | no | Movable. |
| profile_executor | 6144 | `profile_executor_start.c:100` | permanent | no | Movable; control-loop timing needs a bench check. |
| profile_exec_wdt | 6144 | `profile_executor_start.c:206` | permanent | no | Movable. |
| screen_idle | 6144 | `screen_idle.c:357` | permanent | no | Movable. |
| OTA Pico relay | 6144 | `ota_pico_relay.c:760` | transient | no | Movable. |
| ota_confirm | 5120 | `main_network_http.c:780` | boot-transient | yes | Keep. |
| zone_sweep | 5120 | `zones_current_sweep_task.c:2503` | on demand | no | Movable. |
| wifi_prov_owner | 4096 | `wifi_prov.c:714` | permanent | no | Movable. |
| kiln_io_owner | 4096 | `kiln_io_owner.c:664` | permanent | no | Possibly movable. It runs I2C, and any stack-local SPI/I2C DMA buffer would bounce. |
| thermo_owner | 4096 | `thermo_owner.c:245` | permanent | no | Possibly movable. It runs SPI to the MAX31856s, with the same DMA-bounce concern. |
| system_uart_bridge | 4096 | `uart_bridge_system.c:211` | permanent | no | Movable. |
| UI network workers | 4096 each | `ui_page_network.c:273/668`, `ui_page_network_manage.c:404/457/526` | transient | no | Movable. |
| update_fetch_wr | 4096 | `update_fetch.c:663` | per fetch | no | Movable. It counts against the 32752 B fetch admission. |
| ota_http_esp/pico/recovery workers | 2048-4096 | `ota_http_esp.c:832`, `ota_http_pico.c:601`, `ota_http_recovery.c:173/386` | transient | some (recovery) | Keep the recovery-apply worker; the reboot workers are movable but tiny. |
| danger_mode | 3072 | `danger_mode.c:428` | permanent | no | Movable. |
| cfg_autofmt | 3072 | `cfg_fs_mount.c:343` | boot | no | Movable. |
| UART event tasks (x2) | `KILNCTL_UART_OWNER_STACK` 3072 | `hal_uart_esp.c:219` | permanent | no | Movable. |

Already in PSRAM (`*WithCaps`, `MALLOC_CAP_SPIRAM`): gpio_probe,
link_watchdog, the info/io/safety/thermo/touch/ui_test/log bridges,
autotune_engine, safety_poll, telemetry_log, dns_hijack, uart_protocol_rx
(`uart_protocol.c:513`), update_fetch (TLS), factory/sw reset reboot, monitor.

### 2.3 IRAM code that is still in DIRAM (74611 B)

Per archive, in bytes:

| Archive | Bytes |
|---|---:|
| libpp | 14708 |
| libspi_flash | 9474 `.text` + 9022 `.data` |
| libphy | 5150 |
| esp_hw_support | 5122 |
| freertos | 4995 |
| esp_hal_mspi | 4901 |
| heap | 4328 |
| net80211 | 3880 |
| esp_driver_spi | 3571 + 304 |
| esp_hal_gpspi | 2901 |
| esp_psram | 2232 |
| ringbuf | 2122 |
| xtensa | 1984 |
| hal | 1979 |
| libc | 1893 |
| esp_timer | 755 |

The options behind these, and whether each could go:

| Option | Evidence | Can it go under XIP? |
|---|---|---|
| `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`, `ESP_PHY_IRAM_OPT` | libpp + net80211 + libphy, about 23.7 KB. IDF help puts WIFI_IRAM_OPT at about 10 KB and RX_IRAM_OPT at about 17 KB. | These exist for throughput and to keep code off flash during cache-disabled windows. Under XIP the second reason is gone, but code would run from PSRAM at 40 MHz octal (`SPIRAM_SPEED_40M`), so expect lower Wi-Fi throughput. |
| `SPI_MASTER_ISR_IN_IRAM`, `SPI_SLAVE_ISR_IN_IRAM` (no SPI slave is used), `GDMA_CTRL_FUNC_IN_IRAM`, `I2C_MASTER_ISR_HANDLER_IN_IRAM` | esp_driver_spi + gpspi HAL, about 6-7 KB | Only the mmap freeze windows still disable the cache, and those already disable interrupts. `SPI_SLAVE_ISR_IN_IRAM` buys nothing on this board. |
| `LOG_IN_IRAM`, `LIBC_MISC_IN_IRAM`, `LIBC_LOCKS_PLACE_IN_IRAM`, `ESP_TIMER_IN_IRAM` | — | Needed while any IRAM ISR remains. Revisit only together with the row above. |
| `SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM`, esp_hal_mspi, esp_psram, heap | — | Keep. Flash and PSRAM share the MSPI bus. |

## 3. Ranked movable consumers

Saving means internal bytes returned to the heap. Risk is the risk of the
move, not of leaving things as they are.

| # | Consumer | Bytes | Why internal today | Move | Risk | Saving |
|---|---|---:|---|---|---|---:|
| 1 | Wi-Fi static TX buffers (`ESP_WIFI_STATIC_TX_BUFFER_NUM=16`, ~1.6 KB each, allocated at `esp_wifi_init`) | 25600 | DMA, so these must stay internal | Reduce the count to 8. This is not a move. | MED: web/TLS TX throughput; bench soak needed | 12800 |
| 2 | profile_executor + profile_exec_wdt stacks | 12288 | Allowlist reason "writes NVS (cache-disabled windows)", stale under XIP | `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)` | MED: control-loop and guard-9 timing | 12288 |
| 3 | `s_lvgl_task_stack` | 10240 | Comment cites NVS writes (stale). The real blocker is `ui_page_diagnostics.c:678`. | Hop `esp_ota_get_state_partition` to bx_flash_worker (or read it once at boot), then use a PSRAM stack | MED: every LVGL callback must be re-audited for mmap calls | 10240 |
| 4 | http_async_job stacks (transient) | 10240 per job | Allowlist reason "writes NVS (cache-disabled windows)" | PSRAM stack | LOW | 10240 at peak, plus removing the 10 KB contiguous-block demand |
| 5 | `ESP_WIFI_IRAM_OPT` code | ~10000 | Throughput | Disable | MED: Wi-Fi throughput | ~10000 |
| 6 | kiln_cfg_swap worker stack | 8192 | Allowlist reason "writes NVS" | PSRAM stack | LOW | 8192 |
| 7 | `s_ota_esp_chunk` + `s_ota_pico_chunk` | 8192 | Comment: "internal-SRAM, DMA-safe" | Share one buffer (the update claim already serialises users), or allocate on demand | LOW | 4096 (merge) or 8192 (on demand) |
| 8 | `main_boot_ctx_t ctx` (SafetyLink 4520 + uart_proto 1208) | 5728 | Default placement | `EXT_RAM_BSS_ATTR` on separated SafetyLink/proto statics; keep the SPI/I2C device objects internal | MED: safety-link timing; check that no member is handed to a DMA engine | 5728 |
| 9 | screen_idle stack | 6144 | Allowlist reason "drives LVGL/SPI ... cache-disabled flash ops" | PSRAM stack | LOW | 6144 |
| 10 | wifi_prov_owner + danger_mode + system_uart_bridge + 2 UART event stacks | 17408 | Allowlist reasons "writes NVS" or "runs UART drivers during cache-disabled flash ops" | PSRAM stacks | LOW (UART ISR no longer in IRAM) | 17408 |
| 11 | Mid-size statics: `s_exec` 3580, `s_unknown_paths` 2048, `s_degraded` 1920, `s_bulk_pairs` 1846, `json` 1536, `fst_storage` 1512, wifi scan buffers ~4284 | 16726 | Default placement | `EXT_RAM_BSS_ATTR` | LOW | 16726 |
| 12 | `SPI_MASTER/SPI_SLAVE/GDMA/I2C` ISR-in-IRAM code | ~6500 | IRAM-safe ISRs | Disable `SPI_SLAVE_ISR_IN_IRAM` (unused); evaluate the others | LOW (slave) / MED (others) | ~1000-6500 |
| 13 | httpd_worker stack | 8192 | Reaches `esp_ota_end`/partition description | Hop OTA finalisation to an internal worker, then set `config.task_caps` | HIGH | 8192 |
| 14 | bx_flash_worker stack | 10240 | Reaches `esp_core_dump_get_summary` | Split the mmap jobs out | HIGH | up to 10240 |

**Realistic saving (LOW-risk items only):**

| Item (row #) | Bytes |
|---|---:|
| http_async_job (4, peak) | 10240 |
| kiln_cfg_swap (6) | 8192 |
| OTA chunk merge (7) | 4096 |
| screen_idle (9) | 6144 |
| permanent small stacks (10) | 17408 |
| mid-size statics (11) | 16726 |
| SPI slave IRAM (12) | about 1000 |
| **Total permanent** | **about 53.6 KB** (63.8 KB counting the http_async_job peak) |

- **Adding the MED items:** lvgl (10240), executor (12288), ctx (5728),
  Wi-Fi TX buffer count (12800) and WIFI_IRAM_OPT (about 10000) bring it to
  about 104 KB.
- **Conservative planning figure:** about 45-55 KB of idle internal free, if
  the LOW items land with no bench regression. That roughly doubles today's
  ~45 KB idle free.
- **Owner budget:** the fetch admission (32752 B) and the 8192 B min_free
  floor already hold today. These moves add margin; they do not repair a
  breach.

## 4. Runtime internal allocations where PSRAM would do

`heap_caps_malloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` with no PSRAM
attempt first. Each one takes internal heap while a GitHub fetch may be
running, and each one splits the 16 KB largest block.

- **`profile_exec_status_t` (1512 B), 13 sites:**
  - `uart_bridge_ext_control.c:389`;
  - `dashboard_exec_http.c:167`, `:351`, `:430`;
  - `ota_http.c:427`;
  - `safety_cfg_http.c:1580`, `:1797`;
  - `safety_link_frames.c:543`;
  - `screen_idle.c:195`;
  - `ui_page_home_actions.c:85`, `:352`;
  - `ui_page_home_refresh.c:137`;
  - `ui_page_profile_detail.c:304`.

  `profile_executor_get_status()` (`profile_executor_status.c:455`) only
  copies under `s_exec.lock`. Nothing here needs internal RAM.
  `update_fetch_heap.h:109` already budgets for "about 10 paths" of this
  allocation.
- **`diagnostics_http.c:1911`:** `CFGFS_FILE_BODY_MAX` = 8192 B (LittleFS
  read into a buffer; works from PSRAM under XIP). This is the largest
  transient internal request.
- **`kiln_cfg_swap.c`:**
  - `:1064` `kiln_cfg_swap_apply_scratch_t`, 4252 B;
  - `:1276` `kiln_cfg_swap_finish_scratch_t`, 2564 B;
  - `:1417` `kiln_cfg_swap_pending_t`, 1688 B.
- **Firing history:**
  - `profile_executor_firing_stats.c:573`, `:804` and
    `profile_executor_status.c:735`: `profile_firing_history_blob_t`, 1364 B
    each. Their comments say "touches NVS", which is a stale reason under
    XIP.
  - `firing_stats_cfg_fs.c:167` (1368 B) and `:265`.
- **Small items:**
  - `live_profile.c:604`, `PROFILE_BLOB_MAX_SIZE` 456 B;
  - `profile_executor.c:471`, `profile_t` 424 B;
  - `uart_bridge_system.c:69`, `uart_proto_message_t` 268 B;
  - `safety_cfg_http.c:1780`, `safety_link_status_t` 200 B;
  - `safety_cfg_http.c:1831`, 16 B.
- **Not bugs:**
  - PSRAM-first with an internal fallback: `profiles_http.c:329`
    (`profiles_state_t`, 42416 B) and `profile_executor_firing_stats.c:341`.
  - `update_fetch.c:1154` stager scratch: internal on purpose (MED-1, in the
    admission budget).
  - `panel_spi_bringup.c:268` and the `panel_spi_draw.c` text buffer: DMA.

**Driver buffers checked:**

- **LVGL** draw buffers are already in PSRAM (`lvgl_port.c:1245-1246`).
- **mbedTLS** uses `MBEDTLS_EXTERNAL_MEM_ALLOC` and dynamic buffers.
- **lwIP/Wi-Fi** dynamic allocations try PSRAM
  (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). The static RX (10) and TX (16) DMA
  buffers stay internal.
- **UART rings:** RX 2048 and TX 1024 per port (`hal_uart_esp.c:72-73`),
  two ports. They land internal because they are below
  `SPIRAM_MALLOC_ALWAYSINTERNAL` (8192). Under XIP, with the ISR not in IRAM,
  they could be PSRAM, but the IDF UART driver chooses the caps. Lowering
  ALWAYSINTERNAL (proposed to 4096 in `INTERNAL_RAM_DRIFT_2026-10-13.md`)
  does not move them.

## 5. Findings

### HIGH (0)

None. The owner budget is met today. Every item below is margin or a
stale-rule cleanup, not a breach.

### MED (4)

- **MED-1. Transient internal allocations where PSRAM would do (section 4).**
  - 27 sites allocate `MALLOC_CAP_INTERNAL` with no PSRAM attempt for
    task-only data.
  - The worst are `diagnostics_http.c:1911` (8192 B), the
    `kiln_cfg_swap.c:1064` path (4252 B) and the 13
    `profile_exec_status_t` sites (1512 B each, polled by UI and HTTP).
  - Under XIP none of them needs internal RAM.
  - Against the owner's rule these are device bugs: they lower min_free
    and fragment the 16 KB largest block that the update fetch admission
    depends on.
  - **Fix:** `persist_scratch_alloc()` / PSRAM-first.
- **MED-2. Permanent internal task stacks justified by a rule XIP removed
  (section 2.2).**
  - kiln_cfg_swap 8192, profile_executor 6144, profile_exec_wdt 6144,
    screen_idle 6144, wifi_prov 4096, system_uart_bridge 4096, danger_mode
    3072, two UART event tasks of 3072 each: about 43 KB permanently
    internal.
  - None of them reaches an mmap call.
  - The reasons in `tools/check_internal_alloc_allowlist.ps1:21-51` ("task
    writes NVS/flash (cache-disabled windows)", "may run during
    cache-disabled flash ops") no longer describe this build.
- **MED-3. The lvgl stack (10240 B static) stays internal for a stale
  reason, and its real blocker is undocumented.**
  - The comment at `lvgl_port.c:1340-1353` cites NVS writes from LVGL
    callbacks. That is safe under XIP.
  - The actual reason a PSRAM stack would still assert is
    `ui_page_diagnostics.c:678` `esp_ota_get_state_partition()`, an otadata
    mmap, on the lvgl task.
  - Moving the stack without first moving that call would reproduce the
    2026-08-20 class of crash, this time in `esp_cache_utils.c:94` instead of
    `cache_utils.c:126`.
- **MED-4. The internal-alloc gate is blind to explicit
  `MALLOC_CAP_INTERNAL`.**
  - `tools/check_internal_alloc_allowlist.ps1:73` matches plain
    `malloc`/`calloc`/`xTaskCreate*` only.
  - None of the 27 `heap_caps_malloc(MALLOC_CAP_INTERNAL...)` sites in
    MED-1 is gated or allowlisted, so new ones land unseen. (This is a
    tooling gap and is listed for completeness. The device-side fix is
    MED-1.)

### LOW (5)

- **LOW-1.** `s_ota_esp_chunk` and `s_ota_pico_chunk` (`ota_http_esp.c:104`,
  `ota_http_pico.c:65`) are two 4 KB `.bss` buffers. They are only used
  under the single update claim, which already serialises them. One shared
  buffer saves 4096 B with no behaviour change.
- **LOW-2.** About 16.7 KB of task-only statics sit in internal `.bss`
  (section 3, row 11). `EXT_RAM_BSS_ATTR` is already used next to them
  (`safety_cfg_http.c:615`).
- **LOW-3.** `main_boot_ctx_t ctx` (`main.c:246`, 6552 B) is one static
  that mixes DMA-adjacent device objects with 5.7 KB of
  `SafetyLinkClass`/`uart_protocol_t` state. Splitting it would let the
  large part go to PSRAM (MED risk; bench the safety link).
- **LOW-4.** http_async_job (`http_async_job.c:122`) creates a 10240 B
  internal stack per job. That needs a contiguous 10 KB block out of a
  16 KB largest block, and fails if a fetch has fragmented the heap. A PSRAM
  stack is safe: the jobs write NVS, they do not mmap.
- **LOW-5.** `CONFIG_SPI_SLAVE_ISR_IN_IRAM=y` keeps SPI-slave ISR code in
  IRAM, but the firmware has no SPI slave. `ESP_WIFI_STATIC_TX_BUFFER_NUM=16`
  (25.6 KB internal DMA) was never tuned the way RX was (10/6,
  `sdkconfig.defaults` around line 560). Both are candidates for a measured
  reduction.

### INFO (4)

- **INFO-1.** httpd_worker (8192 B) and bx_flash_worker (10240 B) must stay
  internal while OTA finalisation (`ota_http_esp.c:351/383/561`) and the
  crash-report summary (`crash_report.c:582`) run on them. Each needs an
  internal-worker hop before `task_caps`/PSRAM is possible.
- **INFO-2.** The 6866 B merged string pool attributed to
  `spi_flash_os_func_app.c.obj` (map line 47207) holds the string literals
  of noflash/IRAM code, not spi_flash data. It shrinks only alongside the
  IRAM options.
- **INFO-3.** The Wi-Fi/PHY IRAM options (about 23.7 KB) are now a
  throughput choice, not a correctness one. Any change needs an iperf or
  update-download soak.
- **INFO-4.** `dashboard_http.c:135` `esp_image_get_metadata` is primed once
  on app_main (`dashboard_http.c:997`) and cached, so it does not block a
  PSRAM httpd stack.

**Counts:** HIGH 0, MED 4, LOW 5, INFO 4.
