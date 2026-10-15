# REVIEW_RAM_PSRAM_STACKS: adversarial review of the PSRAM task-stack move under XIP

Scope, on origin/dev:
- `a6c671e49`: moves 9 task stacks to PSRAM, merges the OTA chunk buffers, widens the internal-alloc allowlist regex.
- `715ffc0ea`: `check_psram_task_stacks.ps1` strips comments before it matches the UART delete.
- `6880bfefb`: XIP from PSRAM instead of the UART ISR in IRAM.
- `65927acc7`: mark-valid read-back with bounded backoff (AUDIT_XIP_CACHE_DISABLED MED-1).

This is a review only; nothing was fixed. The board was not touched. Everything was checked against ESP-IDF v6.0.2 (`C:\esp\v6.0.2\esp-idf`, the version `dependencies.lock` pins) and against the generated `firmware/KilnFW/sdkconfig`:
- set: `CONFIG_SPIRAM_XIP_FROM_PSRAM`, `SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA`, `FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`, `APP_ROLLBACK_ENABLE`
- not set: `SPI_FLASH_AUTO_SUSPEND`, `SECURE_FLASH_ENC_ENABLED`, `UART_ISR_IN_IRAM`

Result: HIGH 0, MED 0, LOW 3, INFO 7.

This review does not repeat what AUDIT_XIP_CACHE_DISABLED.md (HIGH 0 / MED 1 / LOW 3 / INFO 6) and AUDIT_INTERNAL_RAM_XIP.md already cover.

## Ground rules

These IDF v6.0.2 facts decide every per-task verdict below:

1. **Plain flash ops do not disable the cache under XIP.**
   - `components/spi_flash/spi_flash_os_func_app.c:32` defines `SPI_FLASH_CACHE_NO_DISABLE` as `AUTO_SUSPEND || (SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA) || RAM app`, and this build makes it true.
   - So `esp_flash_*` read, write and erase never disable the cache. Neither do the callers built on them: NVS commit and erase, LittleFS, `esp_partition_read/write/erase_range`, and `esp_core_dump_image_check/erase`.
   - A PSRAM stack is safe for all of them.
   - The IDF documentation says the same: `docs/en/api-reference/peripherals/spi_flash/spi_flash_concurrency.rst:22,43` and `xip_from_psram.inc`.
2. **mmap paths still freeze the cache and assert that the stack is internal.**
   - `esp_mmu_map`/`esp_mmu_unmap` call `s_stop_cache` (`esp_mmu_map.c:383,445,642`), which on S3 is `esp_cache_freeze_caches_disable_interrupts()`.
   - That function asserts `s_task_stack_is_sane_when_cache_frozen()` (`esp_cache_utils.c:96`). Assertions are enabled in this build.
   - So any of the following, called from a PSRAM-stack task, is an abort:
     - `esp_partition_mmap`, `spi_flash_mmap`, `bootloader_mmap`
     - `esp_image_verify`, `esp_image_get_metadata`, `esp_partition_get_sha256`
     - `esp_ota_end`
     - any `read_otadata` user: `esp_ota_set_boot_partition`, `esp_ota_get_boot_partition`, `esp_ota_mark_app_valid_cancel_rollback`, `esp_ota_check_rollback_is_possible`, `esp_ota_get_state_partition`
     - `esp_core_dump_get_summary`
   - Two calls look similar but do not mmap:
     - `esp_ota_get_running_partition` uses `spi_flash_cache2phys` and caches the result.
     - `esp_core_dump_image_check/erase` only use partition read/erase.
3. **`esp_restart` from a PSRAM stack is safe.** On S3, `esp_restart_noos` (`components/esp_system/port/soc/esp32s3/system_internal.c:101-106`) moves SP off an external-RAM stack before it disables the cache.
4. **`xTaskCreatePinnedToCoreWithCaps` creates a static task.** It allocates the TCB with `pvPortMalloc` (internal) and the stack with `heap_caps_malloc(caps)`, then calls `xTaskCreateStaticPinnedToCore` (`components/freertos/esp_additions/idf_additions.c:31-75`).
   - The idle task never frees a static task. A plain `vTaskDelete` on one therefore leaks both blocks.
   - `vTaskDeleteWithCaps(NULL)` handles self-delete through a helper task (`idf_additions.c:152-209`).

## Per-task call-graph verdicts (the 9 moved stacks)

Every moved task requests `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. I enumerated every real mmap-reaching call site in `firmware/KilnFW/App` (listed under INFO-7). None of them is reachable from these tasks.

| Task | Create site | Flash work reached | mmap reachable? |
|---|---|---|---|
| system_uart_bridge | `uart_bridge_system.c:211` | Factory reset: `factory_reset_execute()` (`factory_reset.c:589`) runs its job on bx_flash_worker (internal stack) unless it is already there. The job's NVS and coredump erases do not mmap either. Reboot goes through a WithCaps `reboot_task` and `esp_restart` (ground rule 3). | no |
| profile_executor | `profile_executor_start.c:98` | Runs NVS/cfg writes only through `uart_bridge_ext_run_on_flash_worker` (`profile_executor_status.c:31`). `dashboard_get_status()` reads no flash lazily any more (see the flash-facts row). | no |
| profile_exec_wdt | `profile_executor_start.c:203` | none | no |
| wifi_prov_owner | `wifi_prov.c:719` | NVS (`esp_wifi_set_config`, saved-network writes): safe under ground rule 1 | no |
| kiln_cfg_swap | `kiln_cfg_swap_worker.c:224` | `kiln_cfg_swap_apply()`: zones/safety-cfg NVS and LittleFS writes plus safety-link UART: safe under ground rule 1 | no |
| danger_mode | `danger_mode.c:431` | none (the `esp_restart` mention at :427 is a comment) | no |
| screen_idle | `screen_idle.c:358` | `dashboard_get_status()` (`screen_idle.c:216`), which reads no flash lazily | no |
| hal_uart event task (x2) | `hal_uart_esp.c:226` | none | no |

The one lazy mmap this move had to remove was in `dashboard_get_status()`'s flash facts, which called `esp_image_get_metadata`. It was removed correctly:
- `dashboard_prime_flash_facts()` (`dashboard_http.c:114-138`) runs from `app_main` at `main.c:251`, before `main_boot_early` creates any task.
- `dashboard_get_status()` (`dashboard_http.c:565-590`) only reads the primed values.

## OTA chunk-buffer sharing

There is one `static uint8_t s_ota_esp_chunk[4096]` (`ota_http_esp.c:104`). It is exposed by `ota_http_esp_chunk_buf()` (`:109`), and its users are:
- the ESP upload loop (`ota_http_esp.c:309-330`) and its refusal drains (`:490,501,510`)
- the Pico upload (`ota_http_pico.c:133,241-256`) and its drains (`:426,434,441`)
- WP4 stage upload and status (`update_http.c:127,323,464`)
- the install pre-check (`update_install_http.c:97-99`)

**No two of them can run at once.**
- Every user is a synchronous handler on httpd's single task. A request's `httpd_req_recv`/`esp_ota_write` sequence finishes before httpd dispatches any other request.
- Only three places in the firmware use `httpd_req_async_handler_begin` (`http_async_job`): `backup_import.c:4615`, `diagnostics_http.c:647` (crash clear) and `safety_cfg_http.c:1856,2045`. None of them touches the buffer.
- The fetch path (`update_fetch.c`) writes the stage from its own buffers through `update_fetch_wr` (an internal-stack `xTaskCreatePinnedToCore`, `update_fetch.c:663`). It never borrows this buffer.
- The Pico relay task started after staging does not see the buffer either. `pico_img_stage_write_chunk` copies out of it.

INFO-1 records that this safety rests on the single-task invariant rather than on the claim the comment cites.

## Findings

### LOW-1: `system_bridge_task` self-deletes with plain `vTaskDelete` after the move to WithCaps

`uart_bridge_system.c:75`: when the one-time `heap_caps_malloc(sizeof(*msg), MALLOC_CAP_INTERNAL...)` at `:69` fails, the task calls `vTaskDelete(NULL)`.
- `a6c671e49` changed the creation at `:211` to `xTaskCreatePinnedToCoreWithCaps`, so this path now leaks the internal TCB and the 4096 B PSRAM stack for good (ground rule 4).
- It only runs on an internal-heap failure at boot, which is why it is LOW.
- It should be `vTaskDeleteWithCaps(NULL)`.

### LOW-2: `check_psram_task_stacks.ps1` checks delete pairing only for `hal_uart_esp.c`

`tools/check_psram_task_stacks.ps1:15-23` pins the creation variant and caps of all 8 task entries, but the delete-pairing check (`:71-72`) reads only `hal_uart_esp.c`.

A negtest confirms the gap: `tools\negtest.ps1 -Preset check -PresetArg tools/check_psram_task_stacks.ps1` on base `8af425e10`, baseline PASS, `real_tree_unchanged: true`.

| Mutation | Result |
|---|---|
| `uart_bridge_system.c`: create via plain `xTaskCreatePinnedToCore` | CAUGHT ("created with plain xTaskCreatePinnedToCore (internal stack)") |
| `screen_idle.c`: `MALLOC_CAP_SPIRAM` changed to `MALLOC_CAP_INTERNAL` | CAUGHT ("lacks MALLOC_CAP_SPIRAM") |
| `danger_mode.c`: `vTaskDelete(NULL);` inserted into `danger_mode_task` | **MISSED** (exit 0) |

This gap is how LOW-1 landed with the check green. The fix is to apply the `vTaskDelete(` versus `vTaskDeleteWithCaps(` rule to every listed file, not just the UART one.

### LOW-3: pre-existing WithCaps tasks deleted with plain `vTaskDelete`; one of them leaks internal RAM on every boot

These were not part of the moved 9, but they are in the same class and matter for the internal-RAM budget:
- `uart_bridge_info.c:304`: `info_boot_push_task` (created WithCaps at `:369`) ends with `vTaskDelete(NULL)` on its **normal** path.
  - Every boot leaks one `StaticTask_t` from the internal heap (`pvPortMalloc`, a few hundred bytes) plus 3072 B of PSRAM.
- Error or deinit paths only, so each leaks at most once:
  - `wifi_prov_link.c:1249,1261` (`dns_hijack_task`, created `:1357`): socket or bind failure.
  - `uart_protocol.c:464` (`uart_protocol_rx_task`, created `:513`).
  - `safety_link.c:657-658`, which deletes `poll_task` (created WithCaps at `:594`) externally with `vTaskDelete`.
- `factory_reset.c:170` and `sw_reset_http.c:227` are unreachable after `hal_wdt_reboot()`, so they do not count.

### INFO-1: the chunk-buffer comment cites the wrong invariant

`ota_http_esp.c:95-102` says the buffer is safe because it is "only ever touched while s_update_claim is held". That is false for the refusal drains (`:490,501,510`; `ota_http_pico.c:426,434,441`), which use the buffer precisely because the claim was refused, or before it is taken.

The real invariant is the one stated at `ota_http_esp.c:106-108` and `ota_http_pico.c:58-63`: httpd is one task. Nothing enforces it. If any of these handlers is ever moved onto `http_async_job`, the way backup import was, a refused upload's drain could overwrite another upload's chunk between `httpd_req_recv` and `esp_ota_write`/`psa_hash_update`. No such move exists today.

### INFO-2: stale "must be internal for the flash path" reasoning

`uart_bridge_system.c:64-68` keeps `msg` in `MALLOC_CAP_INTERNAL` because the factory-reset flash path touches it. Under XIP, plain flash ops no longer disable the cache (ground rule 1), and the job runs on bx_flash_worker in any case. The allocation is harmless but its rationale is out of date. Similar internal-only comments elsewhere date from before XIP.

### INFO-3: UART ISR latency without `UART_ISR_IN_IRAM`

Since `6880bfefb` the UART ISR executes from PSRAM through the cache. The only time it cannot run is during an mmap freeze. Those windows are microseconds long and happen at boot, OTA end and otadata reads. Even so, they are far shorter than the time to fill the 128-byte FIFO at 230400 baud (about 5.5 ms).

The event-task stacks that moved to PSRAM do not affect ISR latency. The ISR runs on the interrupt stack and the event task only drains the queue. No finding.

### INFO-4: SPI buffers on PSRAM stacks

None of the moved tasks issues SPI transfers. Callers that do pass stack buffers through `esp_spi_owner.c`:
- MAX31856 uses `.dma_chan = HAL_SPI_DMA_NONE` (`MAX31856.c:388`), so CPU copies are fine from PSRAM.
- The display bus uses DMA (`main_boot_early.c:563`), but it is driven from LVGL's internal `.bss` stack.

A future SPI-from-PSRAM-stack caller on the DMA bus would need `MALLOC_CAP_DMA` bounce buffers.

### INFO-5: the check guards placement, not reachability

`check_psram_task_stacks.ps1` proves that the 8 entries create PSRAM stacks with XIP on. It cannot detect a future mmap call added to one of their call graphs, and that would abort on the stack-sanity assert (ground rule 2). The source comments (`profile_executor_start.c:60-110`, `danger_mode.c:415-430`) are the only guard. This restates AUDIT_XIP_CACHE_DISABLED INFO-5 for the newly moved tasks.

### INFO-6: `mark_unverified` is write-only

`65927acc7` adds the read-back and backoff in `boot_guard.c:684-705`:
- skips of 0, 1, 3, 7 and 15 polls, capped at 15 polls per retry
- after `BOOT_CONFIRM_MARK_MAX_FAILS` (10, `boot_guard.h:422`) failures, it accepts on mark-valid's own `ESP_OK` and sets `mark_unverified`

The logic is sound. `esp_ota_mark_app_valid_cancel_rollback()` on an already-VALID image is a no-op, so a retry cannot rewrite otadata spuriously. `main_ota_read_valid` (`main_network_http.c`) runs on the internal-stack confirm task.

However, nothing outside the host test reads `mark_unverified` (`test_boot_guard.c:885,895`). The firmware never logs or exposes the give-up. The only trace is the per-retry `ESP_LOGW`, and `main_network_http.c:302-309`'s summary line omits the flag.

### INFO-7: inventory of mmap-reaching call sites (all on internal stacks)

- `dashboard_http.c:135`: `app_main`.
- `ota_http_esp.c:351,365,383,561,732,788`: httpd.
- `boot_partition_verify.c:20,24`.
- `recovery_switch.c:41,63,82,103`, reached from `ota_http_recovery.c:343` (httpd) and `main_boot_early.c:423` (boot).
- `crash_report.c:582`: `crash_report_init` from `main_boot_early.c:264`.
- `ui_page_diagnostics.c:678`: LVGL `.bss` stack.
- `main_network_http.c:182,204`: the ota_confirm task, plain `xTaskCreate` 5120 at `:796`.

`update_stage.c:264` (`ota_esp_image_header_check`) only checks a buffer and does not mmap. Anyone who moves a task stack to PSRAM in future must keep it off this list.
