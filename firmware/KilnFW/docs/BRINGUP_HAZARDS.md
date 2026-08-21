# Bring-up hazards and their fixes (KilnFW / ESP32-S3)

Hard-won bugs and root causes found during bring-up, condensed out of
`TODO.md` so the plan file only tracks open work. Dates are when found;
all fixes below are shipped unless marked otherwise.

## Internal-SRAM exhaustion and the Wi-Fi task-registration race (2026-08-20)

`CONTROL`/`PROFILES`/`AUTOTUNE`/`WIFI` bridge tasks (`uart_bridge_ext.c`) and
`gpio_probe.c`'s task used plain `xTaskCreatePinnedToCore()` right after
`wifi_prov` brought up the softAP — exactly the window the Wi-Fi driver is
itself allocating internal-SRAM buffers. Plain task creation always
allocates both TCB and stack from internal SRAM (`CONFIG_SPIRAM_ALLOW_
STACK_EXTERNAL_MEMORY` doesn't change that — only the `*WithCaps` API does).
Proven a *race* (which task failed varied boot to boot with no source
change), not a fixed shortfall. **Fix**: all five creation sites switched to
`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`,
moving stacks to PSRAM (TCB stays internal, but the TCB was never the
contended resource).

**Follow-on: PSRAM-stacked tasks cannot touch flash.** PSRAM is reached
through the flash cache; any such task running while the cache is disabled
(any `nvs_*`/`esp_ota_*` access) crashes with `spi_flash_disable_interrupts_
and_other_cpu / cache_utils.c:126`. Reproduced via `POST /api/zones` then a
profile save. **Fix**: one shared flash-safe executor task with its own
*internal* 8192-byte stack; exposed bridge tasks hand their whole
per-message body to it and block until it returns. Must be started early
(right after `autotune_engine_start()`, before display/LVGL bring-up) — a
lazily-created instance raced `lvgl_port_start()`'s own 8192-byte stack
request for the same shrinking internal block once the MAX31856 ICs were
fitted (added `uart_bridge_ext_start_flash_worker()` as an explicit early
entry point).

**DRAM fragmentation, the underlying cause.** The figure that matters is
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`,
not any free total. `wifi_prov_start()` alone cost 100KB of a 160KB largest
block in one step. Two fixes: LVGL's allocator moved wholesale to PSRAM
(`LV_USE_CUSTOM_MALLOC` + `lvgl_mem_psram.c`), and
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`. Net: largest contiguous internal
block at end of boot went 2560 -> 17408 bytes (6.8x).

**History/autotune-trace buffers were the other half of the 7KB-heap
crisis.** A single zone's history ring buffer was 57.6KB and the autotune
trace 69KB, leaving 7KB of free heap at `app_main` — httpd couldn't start,
Wi-Fi couldn't hold a client. Fixed by packing samples to 8 bytes (history)
and 2 bytes (autotune trace), unpacked at the API boundary.

## Coredump-to-flash was broken, then fixed and proven end to end (2026-08-20)

The `coredump` partition was 64K; this firmware's dump is 82080 bytes
(679200 with `CONFIG_ESP_COREDUMP_CAPTURE_DRAM=y`, needed to resolve task
names/TCBs). Every panic silently failed with `ESP_ERR_NO_MEM` while ESP-IDF
printed "Core dump has been saved to flash." unconditionally regardless of
success — **do not trust that line**; look for `Erase flash N bytes @`
instead. Fixed: `coredump` partition 64K -> 1024K.

Decode command (IDF v6.0.2, run from `firmware/KilnFW`, needs the Espressif
PowerShell profile sourced first):
```powershell
python C:\esp\v6.0.2\esp-idf\components\espcoredump\espcoredump.py `
    --chip esp32s3 -p COM3 info_corefile --off 0x6f0000 `
    --save-core core.bin build\KilnCtrl.elf
```
Notes: `-p COM3` is the USB-Serial-JTAG port (VID:PID 303A:1001), not the
CH340 PC-link port. The ELF must be the exact crashing image — every link
archives its ELF to `build/elf_archive/KilnCtrl-<hash>.elf` for this reason.
Do not decode while the board is crash-looping (a fresh panic corrupts the
partition mid-read). PowerShell reports nonzero exit because esptool writes
progress to stderr — redirect and check the output file, not `$LASTEXITCODE`.
Erase a read dump with `esptool --chip esp32s3 -p COM3 erase-region
0x6f0000 0x80000` or it reports stale on every boot.

**Resolved stack-overflow case, found this way (2026-08-20)**: `wifi_uart_
bridg` task overflowed its 3072-byte stack (3440 used) while the host
polled `wifi_get_status`. The 3072 came from an earlier "internal-SRAM
workaround" shrink of the wifi/autotune bridge stacks from 4096 — a
workaround that freed nothing, since those stacks were already on PSRAM.
Fixed by sizing from measured figures (wifi -> 8192, autotune -> 4096) in
`uart_bridge_ext.c`. Lesson: the panic handler writes to the raw
USB-Serial-JTAG console, which `uart_log_bridge` never sees — a
reset-reason log line only tells you *that* a panic happened, never where;
enabling coredump is what actually finds it.

## UART link congestion (2026-08-20)

`uart_log_bridge_task()` forwarded every `ESP_LOGx` line — including a
safety-link retry warning — over the PC-facing UART with up to 10 retries
at 200ms each (2000ms holding the shared `tx_lock`), starving real PC
replies. Fixed with `uart_protocol_send_limited()` (explicit max-retries
param), log bridge capped to 1 retry. Also added exponential backoff on the
safety poll task's sleep while `no_reply_streak` climbs.

## Wi-Fi mode-switch ordering bug (2026-08-13)

`esp_wifi_set_config(AP)` failed with `ESP_ERR_WIFI_MODE` on every boot:
`wifi_prov_start()`/`ap_fallback_timer_cb()`/`wifi_prov_set_mode()`'s
AP-switch branch all called `esp_wifi_set_config()` before
`esp_wifi_set_mode()` in some branches — the driver rejects a config for an
interface the current mode doesn't include yet. Fixed by reordering
`esp_wifi_set_mode()` first, config applied only on success, in all three
call sites.

## THERMO surfaces disagreeing about the same missing hardware (2026-08-20)

With no thermocouple daughterboard attached, three THERMO UART subcommands
disagreed about the same "channel never came up" state: `thermo_read`
correctly reported invalid; `thermo_read_faults` fabricated a "no faults, SR
0x00" reading from zero-initialized locals because `uart_bridge.c` discarded
the owner call's `esp_err_t` with `(void)`; `thermo_read_reg` (a query
subcommand) silently dropped its reply entirely because it shared the
non-query "no reply needed" error tail. Fixed: faults payload now omits a
failed channel (report absent, not zero) matching `MAX31856_read_all()`'s
own convention; `READ_REG` always replies now, with a 0-length data field
when the owner-side read failed, mirroring how READ/READ_FAULTS encode
failure in-payload rather than dropping the reply. General lesson: a query
subcommand must never share an error path designed for a fire-and-forget
command — "no reply" is only a correct signal when the transport ACK is the
only confirmation that command needs.

## LVGL status-bar icon touch clipping (2026-08-20)

Found wiring the shared LCD top-bar (`App/drivers/ui_topbar.c`), by measuring
on hardware rather than trusting the layout: `ui_theme_apply_touch_area()`
cannot rescue a child whose parent clips it. LVGL hit-testing descends the
widget tree, so a child's extended click area can never reach outside a
parent that does not itself contain the point. A 21x23px gear glyph inside a
32px status bar stayed hard-capped at that 32px of vertical touch no matter
how far the helper extended its nominal reach — verified by injected touch
(a point inside the glyph's extra reach but outside the 32px bar missed).
**Fix**: a floating proxy button parented to the page **root**, not the
status bar, so nothing clips its extended area.

Second trap hit in the same pass: a plain child of a flex-column page root
joins the layout flow. `LV_OBJ_FLAG_FLOATING` is required for a true overlay;
without it the proxy button gets positioned by the flex layout instead of the
intended alignment and consumes real content height (measured: content
region shrank from 267px to 227px when this was first got wrong). Full detail
and the call-site comment are in `ui_topbar.c`.

## Full detail

Dated, hardware-verification-tagged entries for all of the above (and more)
live in `docs/PROJECT_STATUS.md`. This file exists to keep the *recurring,
reusable* lessons in one place rather than buried in a changelog.
