# Audit: KilnFW under XIP from PSRAM, ISRs and cache-disabled paths (2026-10-14)

Scope: `firmware/KilnFW`, plus the `firmware/hwAbstraction/esp` and
`firmware/CommonFW` code it links, built from origin/dev `153c12cf5` with
`CONFIG_SPIRAM_XIP_FROM_PSRAM=y` and `CONFIG_UART_ISR_IN_IRAM` unset
(`firmware/KilnFW/sdkconfig.defaults:628-629`). IDF v6.0.2. This is a static
audit only: no code was changed and the board was not touched.

Build used for section placement: `check_00_kilnfw_target_build.ps1` in
worktree `C:\wt\auxipisr_5jjuni` (output `C:\wt\checkbuild_476648504d`, PASS).
Its `build/config/sdkconfig.h` has `CONFIG_SPIRAM_XIP_FROM_PSRAM 1`,
`CONFIG_SPIRAM_FETCH_INSTRUCTIONS 1` and `CONFIG_SPIRAM_RODATA 1`, and no
`CONFIG_UART_ISR_IN_IRAM`, so REVIEW_XIPLAND MED-1 (a stale sdkconfig building
without XIP) did not apply to this build. Addresses below come from
`xtensa-esp32s3-elf-nm` on `KilnCtrl.elf`, and sections from `KilnCtrl.map`.
`0x4037xxxx` is `.iram0.text` (internal). `0x42xxxxxx` is `.flash.text`, which
under XIP executes from the PSRAM copy.

Related documents, not repeated here: `XIP_PSRAM_INVESTIGATION.md` (risks 1-7)
and `REVIEW_XIPLAND.md` (MED-1 stale sdkconfig, LOW-2 forced-panic coredump not
run, INFO-7 DMA, INFO-8 `hal_kv_esp.c:471`, INFO-9 UART ISR stalls).

Counts: HIGH 0, MED 1, LOW 3, INFO 6.

## Mechanism summary

- **SPI1 erase and write no longer disable the cache.**
  `spi_flash_os_func_app.c:32` defines `SPI_FLASH_CACHE_NO_DISABLE` when both
  `SPIRAM_FETCH_INSTRUCTIONS` and `SPIRAM_RODATA` are set. `spi1_start`/
  `spi1_end` (`:115-199`) then only take `s_spi1_flash_mutex` (`:133`, `:181`).
  The other CPU keeps running and non-IRAM interrupts stay enabled during
  `esp_flash_erase_region`/`esp_flash_write`.
- **mmap still stops the cache, briefly.** `esp_mmu_map.c:379-387`
  (`s_stop_cache`) calls `esp_cache_freeze_caches_disable_interrupts()`
  (`esp_cache_utils.c:94-106`). That function:
  - asserts the calling task's stack is internal;
  - enters a spinlock critical section;
  - disables non-IRAM interrupts on the current core;
  - freezes the external-memory cache, so a cache miss on the other core
    hangs until the unfreeze.

  The window covers one MMU update per map or unmap.
- **The panic and coredump path still suspends the cache** around each flash
  operation, through `spi_flash_os_func_noos.c`. This is unchanged from before
  XIP.
- **No other cache-disable caller is live.** `spi_flash_disable_interrupts_caches_and_other_cpu`
  is still linked (`0x403753e4`). The source search found two other users:
  - the `g_flash_guard_default_ops` guard (`flash_ops.c:84`), which only tests
    and the ESP32 `cache_sram_mmu.c` call;
  - the S3 PSRAM temperature-tuning path (`mspi_timing_by_mspi_delay.c:717`),
    which needs `CONFIG_SPIRAM_TIMING_TUNING_POINT_VIA_TEMPERATURE_SENSOR`.
    That option is not set here, because PSRAM runs at 40 MHz.

## MED

### MED-1: mmap'd flash reads can now race a concurrent SPI1 erase or program and return wrong data; the boot OTA confirm is one-shot

- **Where:**
  - `firmware/KilnFW/App/main_network_http.c:234`. `esp_ota_mark_app_valid_cancel_rollback()`
    runs once, with no read-back. `rollback_cancel_attempted = true` at `:254`
    stops any retry.
  - The other mmap readers are listed under LOW-1 and LOW-2.
  - IDF side:
    - `esp_ota_ops.c:77-98`: `read_otadata` uses `esp_partition_mmap` and then
      memcpy.
    - `esp_ota_ops.c:1068-1101`: `esp_ota_current_ota_is_workable`.
    - `esp_ota_ops.c:614-628`: `rewrite_ota_seq` erases the entry's sector and
      rewrites it from the in-RAM copy.
- **Scenario:**
  - Before XIP, every SPI1 erase or program disabled the cache and stalled the
    other CPU. No task could read flash through an mmap window while the chip
    was busy.
  - Under XIP, only `s_spi1_flash_mutex` is taken. `esp_partition_mmap` and
    the cache reads that follow it do not take that mutex.
  - Other tasks can hold the flash chip busy with an erase (up to hundreds of
    ms for a 64 KB block) or a page program:
    - NVS commits (zones, profiles, run_state, Wi-Fi driver `nvs.net80211`);
    - LittleFS `cfg` writes;
    - the stage writer (`update_http.c:55,60`);
    - `pico_img_stage.c:98,106`.
  - Without auto-suspend (`CONFIG_SPI_FLASH_AUTO_SUSPEND` is not set), an SPI0
    read issued while the chip is busy returns unpredictable data. IDF states
    this for DMA readers in `spi_flash_concurrency.rst`, "When DMA Read Data
    from Flash". A cache refill uses the same SPI0 path.
  - The highest-value case is the first boot after an update. The recovery
    image's `esp_ota_set_boot_partition(app)` writes state NEW, and the
    bootloader marks the image PENDING_VERIFY. `main_ota_rollback_confirm_task`
    then calls `esp_ota_mark_app_valid_cancel_rollback()` while Wi-Fi, NVS and
    LittleFS boot-time writers are active on either core.
- **Consequence:**
  - If the active otadata entry is misread, its CRC fails.
    `bootloader_common_get_active_otadata` then picks the other entry, or none.
    - With none, the call returns `ESP_FAIL` ("Running firmware is factory").
      The task logs it once and never retries.
    - With the older entry, `rewrite_ota_seq` writes VALID into the older
      sector. The newer entry is left PENDING_VERIFY.
  - Either way, the running image stays PENDING_VERIFY on flash. At the next
    reset, `bootloader_utility.c:397-402` marks it ABORTED. With one app slot
    and a factory partition, `:406-413` then boots `factory`, which is the
    recovery image.
  - Result: after a successful update and a healthy boot, the next ordinary
    reset can land the board in recovery mode with no fault recorded. It is
    recoverable with `recovery_exit`, and safety is not affected.
  - The window per call is small (two 32-byte copies), so the failure is rare
    and timing-dependent. It is not bench-proven.
- **Fix (do not apply here):**
  - After `esp_ota_mark_app_valid_cancel_rollback()`, read back
    `esp_ota_get_state_partition(running)` and retry until it reads VALID,
    instead of a one-shot attempt. This uses the same verify-then-retry pattern
    as `boot_guard_mark_healthy()`.
  - More generally, serialize every mmap-reaching IDF call against app flash
    writers. One option is a firmware-wide flash mutex taken by the NVS/cfg/stage
    writers and by the mmap callers, or routing them through the existing
    flash-safe worker.
  - Alternatively, evaluate `CONFIG_SPI_FLASH_AUTO_SUSPEND` once the flash ID is
    known. With it, the hardware arbitrates SPI0 reads against an erase.

## LOW

### LOW-1: esp_image_verify and otadata read-backs can fail spuriously during a concurrent erase (fail-closed)

- **Where:**
  - `firmware/KilnFW/App/drivers/persist/recovery_switch.c:41`:
    `esp_image_verify`, which goes through `bootloader_mmap`
    (`esp_image_format.c`).
  - `firmware/KilnFW/App/drivers/persist/boot_partition_verify.c:20,24`:
    `esp_ota_set_boot_partition` runs `image_validate` and then
    `esp_image_verify`. `esp_ota_get_boot_partition` uses `read_otadata`.
  - `firmware/KilnFW/App/drivers/http/ota_http_esp.c:351`: `esp_ota_end`
    verifies the image through mmap.
- **Scenario:** the same race as MED-1. A misread sector makes a SHA or
  checksum mismatch, or makes the otadata read-back disagree.
- **Consequence:** `recovery_enter`, an ESP OTA, or a boot-partition change is
  refused with an image-invalid or mismatch error even though flash is good.
  Every site fails closed, and a retry normally succeeds. A misread cannot
  produce a false PASS, because a SHA-256 would have to match. Operator
  confusion only.
- **Fix:** retry these verifies once on failure before reporting. Better, take
  the same flash serialization as MED-1.

### LOW-2: The boot-time crash summary can be captured from a misread coredump and persisted

- **Where:**
  - `firmware/KilnFW/App/drivers/safety/crash_report.c:582` calls
    `esp_core_dump_get_summary()`, which uses `esp_partition_mmap`
    (`core_dump_elf.c:910,1007-1013`).
  - The presence check `hal_sysinfo_esp.c:210` calls `esp_core_dump_image_check`.
    That function uses SPI1 `esp_partition_read` (`core_dump_flash.c:363,382`),
    so it is safe.
- **Scenario:** a coredump is present at boot. The summary is parsed through an
  mmap window while another task's NVS or cfg erase is in flight. The record is
  captured once per boot and then persisted.
- **Consequence:** a wrong PC, backtrace or task name is stored as the
  permanent crash record. The parser follows offsets taken from the image, so a
  misread could also send it out of bounds inside the mapped window.
- **Fix:** capture the summary before the NVS, cfg and Wi-Fi writers start, or
  under the MED-1 serialization. Retry when the summary's ELF checksum or
  version check fails.

### LOW-3: panic_handler now executes from PSRAM

- **Where:** `panic_handler` at `0x42005d70`, `.text.panic_handler` in
  `.flash.text` (`KilnCtrl.map:54179`). `esp_core_dump_write` is at
  `0x42012f2c`, also `.flash.text`. `CONFIG_ESP_PANIC_HANDLER_IRAM` is not set.
- **Scenario:** before XIP these ran from flash through the cache, independent
  of PSRAM. Now any fault whose cause is PSRAM itself prevents the panic
  handler from running at all. Examples are a PSRAM timing or cache error, an
  MSPI hang, or a panic raised on one core while the other core holds a cache
  freeze and the handler misses in the cache.
- **Consequence:** no panic text, no coredump and no crash record. The board
  waits for the RTC or interrupt watchdog to reset it. That reset then looks
  like a plain WDT reset, which feeds boot_guard. Faults not caused by PSRAM
  behave as before. Untested, as REVIEW_XIPLAND LOW-2 notes for the coredump
  path.
- **Fix:** evaluate `CONFIG_ESP_PANIC_HANDLER_IRAM=y`, measuring its internal
  RAM cost against the 8192 B floor, and run the forced-panic coredump check
  under XIP.

## INFO

### INFO-1: ISR inventory; no handler flagged ESP_INTR_FLAG_IRAM depends on PSRAM

| Handler | Address / section | Registration | Notes |
|---|---|---|---|
| `io_bridge_isr` (`uart_bridge_io.c:68`) | `0x403774ec` `.iram1.0` | `gpio_install_isr_service(0)` `:692`, `gpio_isr_handler_add` `:698` | calls `xQueueGiveFromISR` (`0x40380ee8`, IRAM). `ctx` (`:628`) in `.dram0.bss` (`0x3fca0659`) |
| `gpio_intr_service` (IDF) | `0x40378770` IRAM | flags 0 | not IRAM-flagged |
| `uart_rx_intr_handler_default` (IDF) | `0x4200b104` `.flash.text`, so PSRAM | `hal_uart_esp.c:187-189`, intr flags 0 | expected with ISR_IN_IRAM unset; `uart_hal_read_rxfifo` `0x4218fcac`, also PSRAM |
| `spi_intr` (IDF spi_master) | `0x40377ca0` IRAM | `CONFIG_SPI_MASTER_ISR_IN_IRAM=1` | firmware sets no `pre_cb`/`post_cb` (`esp_spi_owner.c`), so nothing non-IRAM is reached from it |
| `i2c_master_isr_handler_default` (IDF) | `0x4037ad28` IRAM | flags 0 (no `I2C_ISR_IRAM_SAFE`) | touch FT6336U is polled; no firmware ISR |
| `timer_alarm_isr` (esp_timer) | `0x40377124` IRAM | IDF internal | firmware callbacks (`lvgl_port.c:1207` `lv_tick_timer_cb` `0x42074a18`, `wifi_prov.c:577,594`, `dram_watch.c:47`) use task dispatch |
| `esp_cache_freeze_caches_disable_interrupts` / `s_stop_cache` | `0x4037eeb4` / `0x403787ac` IRAM | | |
| `spi1_start` / `spi1_end` | `0x40375334` / `0x40375348` IRAM | | |

The firmware itself has no `ESP_INTR_FLAG_IRAM`, `DRAM_ATTR` or
`esp_intr_alloc`, no LEDC fade ISR, and no gptimer, RMT, PCNT, MCPWM or touch
ISR. The only firmware `IRAM_ATTR` is `io_bridge_isr`. No registration claims
IRAM safety for a handler that is not fully in IRAM.

### INFO-2: io_bridge_isr's IRAM_ATTR buys nothing, and its comment is misleading

`uart_bridge_io.c:67` says the handler "stays callable with the flash cache
disabled". The GPIO ISR service is installed with flags 0 (`:692`), so it is
masked on that core whenever non-IRAM interrupts are disabled. The 0x1a bytes
of IRAM are harmless, but the comment should say the handler is not IRAM-safe
as registered. Under XIP that matters even less, because erases no longer mask
it.

### INFO-3: Can a UART frame now be lost during an app-side stage erase or write? No, not from the erase itself

- `update_http.c:55,60` (stage erase and write) and `pico_img_stage.c:98,106`
  go through `esp_partition_erase_range`/`esp_partition_write`, then
  `esp_flash_*` and `spi1_start`. Under XIP that only takes the mutex
  (`spi_flash_os_func_app.c:132-133`). The cache stays on, both cores run, and
  the non-IRAM UART1 ISR (in PSRAM) keeps draining the FIFO for the whole
  150-400 ms erase-ahead that drove the original `CONFIG_UART_ISR_IN_IRAM` pin.
- **Remaining loss conditions.** The ISR uses IDF defaults: `rxfifo_full_thresh`
  120 (`uart.c:67,2093`) and RX timeout 10 symbols. `hal_uart_esp.c` overrides
  neither. A byte takes about 43.4 µs at 230400 8N1 and the FIFO is 128 B. With
  a kilnlink frame longer than 120 B (payload up to 253 B,
  `kilnlink_frame.h:28`), the ISR must start within about 8 bytes, or 347 µs,
  of the threshold interrupt. In general the budget is
  (128 - bytes already buffered) x 43.4 µs. A frame is lost only when the ISR
  start is delayed past that budget, by:
  1. a spinlock critical section or interrupts-masked stretch on the ISR's
     core;
  2. higher-priority ISRs;
  3. `esp_mmu_map`/`unmap` freezes, a few µs each, though `esp_image_verify`
     (recovery_enter, `esp_ota_end`) and `read_otadata` do many back to back,
     and a freeze on the other core stalls this core's PSRAM misses;
  4. cold PSRAM instruction and rodata refills for the ISR, about tens of µs
     at 40 MHz octal;
  5. MSPI bus contention from SPI1 page programs and status polls sharing the
     lines with PSRAM.

  None of these is the 64 KB erase, and none comes near 347 µs on its own by
  static reasoning. The bench run of 445 frames during a fetch with 0 link
  errors (REVIEW_XIPLAND INFO-9) agrees. The panic and coredump path masks
  interrupts anyway. The recovery image is a separate non-XIP build.

### INFO-4: DMA and pointer-capability assumptions are unchanged in effect

`esp_ptr_external_ram()` is now true for XIP rodata. The firmware never applies
it to rodata:
- `hal_kv_esp.c:471` and `profiles_http.c:320,1411` probe a stack address
  (REVIEW_XIPLAND INFO-8).
- `panel_spi_bringup.c:268` allocates `MALLOC_CAP_DMA` explicitly.

Flash rodata was not DMA-capable before XIP either, so `spi_master` bounces a
const tx buffer exactly as it did. The one new edge is that
`SPI_TRANS_DMA_USE_PSRAM` (`esp_spi_owner.c:170`) would now also apply to a
const buffer. It is gated by `CONFIG_KILNCTL_SPI_DMA_USE_PSRAM`, which defaults
to n. Review that gate if it is ever enabled.

### INFO-5: Tasks reaching mmap have internal stacks; PSRAM-stack tasks must stay off these paths

`esp_cache_freeze_caches_disable_interrupts` asserts on a PSRAM stack
(`esp_cache_utils.c:96`). The current mmap callers all have internal stacks:
- the confirm task;
- httpd (`esp_ota_end`, `recovery_switch`);
- LVGL `ui_page_diagnostics.c:678`, which uses static `.bss`
  `s_lvgl_task_stack` (`lvgl_port.c:1354`);
- boot (`crash_report`).

The PSRAM-stack tasks are:
- the bridge ext tasks (`uart_bridge_ext.c:185`);
- `info_uart_bridge` (`uart_bridge_info.c:347`);
- `link_watchdog` (`uart_bridge.c:552`);
- `gpio_probe` (`gpio_probe.c:314`).

None of these reaches an mmap path today. Under XIP, NVS and SPI1 writes from a
PSRAM stack no longer assert, but the mmap paths still do. Any plan to move
more stacks to PSRAM (`XIP_PSRAM_INVESTIGATION.md` section 2) must keep those
tasks off every `esp_ota_*` otadata read, `esp_image_verify`, `esp_partition_mmap`
and coredump-summary call.

### INFO-6: The LCD diagnostics OTA state can show a transient wrong value

`ui_page_diagnostics.c:678` calls `esp_ota_get_state_partition()`, which uses
`read_otadata` and therefore mmap, from the LVGL task. Under the MED-1 race it
shows "n/a" or a wrong state for one refresh. Display only.
