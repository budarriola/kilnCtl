# XIP from PSRAM as an alternative to CONFIG_UART_ISR_IN_IRAM (investigation, 2026-10-13)

Status: investigation only. Owner decision 2026-10-13: no build or sdkconfig
change lands from this work, and the bench board was not touched. Every number
below is a static link-map measurement from four target builds of origin/dev
`7c5c63988` (worktree `C:\wt\xipinv_95fur3`, through
`check_00_kilnfw_target_build.ps1`) unless it is marked as an estimate.

## Question

KilnFW is short of internal RAM. Idle internal free is about 34.7 KB on the
bench, the GitHub fetch admission needs 32752 B (`update_fetch_heap.h`), and the
owner floor is internal `min_free >= 8192 B`. `ac18ba56b` set
`CONFIG_UART_ISR_IN_IRAM=y` so the kilnlink UART keeps draining while a flash
erase has the cache off (`docs/audits/LINK_CRC_DURING_FETCH.md`). That fix cost
about 11.7-12.4 KB of internal RAM: bench idle internal free was 47191 B before
it (benchupd4) and 34783 B after it (benchupd5). Is
`CONFIG_SPIRAM_XIP_FROM_PSRAM` a way to keep the link clean during flash writes
without spending that internal RAM?

## 1. What XIP requires here

- IDF v6.0.2, ESP32-S3 N16R8: 16 MB flash (DIO, 80 MHz), 8 MB octal PSRAM at
  40 MHz (`CONFIG_SPIRAM_MODE_OCT`, `CONFIG_SPIRAM_SPEED_40M`).
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` (`components/esp_psram/esp32s3/Kconfig.spiram`)
  selects `SPIRAM_FETCH_INSTRUCTIONS` and `SPIRAM_RODATA`. It defaults to n and
  is not marked experimental on the S3. It needs no bootloader change and no
  partition change.
- At startup the app (`esp_psram.c`, `s_xip_psram_placement`) copies `.rodata`
  and then `.text` from flash into PSRAM with `Cache_Flash_To_SPIRAM_Copy` and
  remaps the cache onto the copies. If they do not fit, boot aborts with
  "No enough psram memory". The unused tail of each 64 KB page goes back to the
  heap.
- The effect that matters is in `spi_flash_os_func_app.c:32`:
  `SPI_FLASH_CACHE_NO_DISABLE` is true when `SPIRAM_FETCH_INSTRUCTIONS &&
  SPIRAM_RODATA`. SPI1 erase and write then take a mutex instead of calling
  `cache_disable()`. The cache, the other core and non-IRAM interrupts keep
  running during an erase. So a UART ISR that is *not* in IRAM still drains the
  128 B FIFO during the 150-400 ms erase-ahead that caused the link CRC errors.
  The IDF docs (`xip_from_psram.inc`, `external-ram.rst`) state that ISRs no
  longer need IRAM under this option.
- `CONFIG_UART_ISR_IN_IRAM` is a global flag covering both UART ports (the
  kilnlink and the PC link). When it is set, `uart.c` allocates both drivers'
  rings, `rx_data_buf`, queues and semaphores with
  `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`. With it off they use
  `MALLOC_CAP_DEFAULT`, which on this config resolves to PSRAM, as the
  pre-`ac18ba56b` bench numbers showed. `hal_uart_esp.c` uses RX 2048 B and
  TX 1024 B rings.
- `check_sdkconfig_defaults_applied.ps1` pins `CONFIG_UART_ISR_IN_IRAM='y'`, so
  any change here must also edit that pin.
- The recovery image (`firmware/KilnFW_recovery`) has its own sdkconfig and is
  unaffected.

## 2. Measured internal RAM effect (static)

Measured with `esp_idf_size` on `KilnCtrl.map`. "DIRAM used" is static internal
RAM: IRAM text plus `.data` plus `.bss`, out of a 341760 B pool. The 16384 B
IRAM block is the fixed cache reservation and is the same in every variant.
Variant C and D were verified via `build/config/sdkconfig.h`. A temporary edit
to the worktree `sdkconfig.defaults` was needed for them because the defaults
override `sdkconfig` during the check's reconfigure. That edit was reverted.

| Variant | XIP | UART ISR in IRAM | DIRAM used (B) | Flash code (B) | Flash data (B) |
|---|---|---|---|---|---|
| A (dev tip) | no | yes | 181906 | 1686840 | 1158960 |
| B | yes | yes | 183013 | 1687052 | 1159184 |
| C | yes | no | 180205 | 1690056 | 1159264 |
| D (pre-`ac18ba56b` shape) | no | no | 179098 | 1689756 | 1159040 |

Derived:

- XIP itself costs **+1107 B** static internal (B-A, and C-D). By archive:
  `libesp_psram` +668 B DIRAM and +102 B IRAM; `libspi_flash` +303 B `.data`.
- The UART ISR's static IRAM/DRAM cost is **2808 B** (A-D, and B-C).
- C against A: **-1701 B** static.
- Most of the ISR flag's cost is not static. It is the runtime ring and buffer
  allocation that moves from PSRAM to internal: 2 ports x (2048 RX + 1024 TX +
  `rx_data_buf` + queues and objects), about 8-9 KB. Static plus runtime matches
  the 12.4 KB bench delta.
- **Estimate for variant C idle internal free:** 34.7 KB + 12.4 KB - 1.1 KB is
  about **46 KB**, so about +11 KB over today. The 32752 B fetch admission would
  then have about 13 KB of margin instead of about 1.9 KB. This is an estimate
  and needs a bench reading.

Not measured, but opened up by XIP: task stacks currently kept internal only
because they write NVS or flash with the cache off
(`INTERNAL_RAM_DRIFT_2026-10-13.md`) could move to PSRAM, provided they never
reach an mmap path (see risk 2). `ESP_WIFI_IRAM_OPT`/`ESP_WIFI_RX_IRAM_OPT`
could also be revisited.

PSRAM cost: about 2.85 MB of code and rodata (page-rounded) is copied into the
8 MB PSRAM, on top of 151420 B of external `.bss`. About 5 MB is left for LVGL,
TLS and the heap. The app partition is 4 MB, so an image that fits the
partition also fits in PSRAM with room to spare.

## 3. Risks

1. **Boot time.** The app copies about 2.9 MB from DIO 80 MHz flash (about
   20 MB/s at best) before `app_main`. Estimate +150-300 ms. Watch the boot_guard
   and S6a startup grace, which are sensitive to ESP startup timing.
2. **The cache is still disabled on some paths.** `esp_mmu_map.c:385` (the
   mmap path) briefly disables the cache and asserts that the calling task's
   stack is internal. `bootloader_mmap`/`esp_image_verify` do the same, and
   `esp_ota_end`, stage verify and `recovery_switch.c:41` use them. The panic and
   coredump write path (`spi_flash_os_func_noos`) is the same as today. A
   non-IRAM UART ISR is blocked during those windows, but they are short image
   or mmap reads, not 64 KB erases.
3. **Concurrency changes.** Today an erase suspends the other core. Under XIP
   other tasks keep running during erase and write. A task reading flash through
   an mmap'd region (SPI0) while SPI1 erases the same chip is serialized by the
   driver's lock, but this is a real behavior change and has not been exercised
   on this firmware. Startup and fetch timing also shift.
4. **`esp_ptr_external_ram()` becomes true for const data.**
   `esp_psram_check_ptr_addr` treats the XIP rodata and text regions as
   external RAM. Any code that checks a pointer's DMA capability will now treat
   a `const` buffer as PSRAM, for example the panel SPI/LCD transmit of a const
   image or table. That code must copy to internal memory or fail.
5. **Shared PSRAM bus.** Instruction and rodata cache misses now refill from the
   same octal PSRAM that holds the LVGL draw buffers, TLS buffers and external
   `.bss`. Octal at 40 MHz DDR is about 80 MB/s against about 20 MB/s for DIO
   flash, so refills may get faster, but LVGL frame time and TLS throughput can
   go either way under contention. 80 MHz PSRAM is a possible follow-up.
6. **The UART ISR runs from PSRAM.** It still runs during erase, but a cache miss
   costs more than an IRAM fetch. At 230400 baud the FIFO lasts about 5.5 ms, so
   this should be ample. The bench must still show zero link errors.
7. **Unproven combination.** XIP has not been run on this board. A failure shows
   up at boot (copy abort or a crash), and the JTAG `flash_firmware()` path still
   recovers from that.

## 4. What a bench trial must prove

Run variant C (XIP on, `CONFIG_UART_ISR_IN_IRAM` off) as a JTAG dev build. Use
A (dev tip) as the control, on the same board and the same day.

- Boot reaches healthy, the boot_guard counter clears, and there is no S6a/S6b
  trip beyond the expected dual-reflash one. Measure the time to `app_main` from
  the boot log.
- Idle internal free from `get_heap_status`. Expect about 46 KB against about
  34.7 KB.
- `update_stage_release` / GitHub fetch:
  - fetch `min_free` and largest free block, against 14267 B / 7936 B under A;
  - safety link counters during the fetch: crc, len and timeouts must be 0. The
    control had 14/8/12/28 errors without the ISR fix.
- Stage verify, `recovery_enter` plus `recovery_apply_staged`, and boot of the
  new app. This covers the mmap and `esp_image_verify` paths.
- NVS and LittleFS writes (zones and profile save) during a firing, with link
  counters and PID tick jitter watched.
- LVGL frame time and touch response on the LCD (`capture_lcd.ps1` plus the LCD
  suite).
- TLS fetch throughput against A.
- A forced panic produces a readable coredump and crash report.
- `latency_soak` for HTTP p95 against A.

## 5. Recommendation

XIP from PSRAM is a credible zero-internal-cost alternative to
`CONFIG_UART_ISR_IN_IRAM`. On its own it costs 1.1 KB of internal RAM. The
saving comes from turning the UART ISR flag off, which XIP makes safe: the
estimate is about +11 KB of idle internal free (about 46 KB), which also
removes the fetch's 1.9 KB admission margin problem. The static numbers alone
(-1.7 KB) understate this, because most of the cost is the runtime rings.

Do not land it from static numbers. The next step is a bench trial of variant C
against the section 4 checklist. Land it only if link errors stay at 0 during the
fetch and the boot, OTA and recovery paths stay healthy. The change itself is
two lines in `sdkconfig.defaults` (`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`, UART ISR
not set) plus the `check_sdkconfig_defaults_applied.ps1` pin.

There is one other zero-RAM route, `CONFIG_SPI_FLASH_AUTO_SUSPEND`, which also
keeps the cache enabled during erase. It depends on the flash chip model, which
is not known without the boot log's flash ID. Check the ID before choosing
between the two routes. AUTO_SUSPEND has no PSRAM, boot-time or DMA-pointer
side effects, but IDF restricts it to specific chips.
