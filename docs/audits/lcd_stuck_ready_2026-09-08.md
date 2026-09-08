# LCD stuck at "kilnCtl ready" (2026-09-08)

## Symptom
Owner reports LCD stuck at a black screen showing "kilnCtl ready". Board itself
is healthy: HTTP fully up on `192.168.1.156`, uptime climbing (750 s at time of
check), running `fc90f682`/`factory`.

## What draws the text, and what should follow
`ILI9488_start()` (`firmware/KilnFW/App/drivers/hw/panel_spi_bringup.c:448-451`
and `:471-474`) draws the banner directly via raw SPI text calls
(`ILI9488_set_text_style`/`ILI9488_printf(disp, "kilnCtl ready")`) during early
panel bring-up, before LVGL exists. This is meant to be transient: once
`lvgl_port_start()` (`main_bridges_bringup.c`) runs, LVGL takes ownership of the
panel and repaints it with the real home screen (`ui_page_home.c`). The banner
is only ever supposed to be visible for the few hundred ms between panel init
and LVGL's first flush.

## Root cause: this is recovery mode working as designed, not a display bug
`GET /api/ota/esp/status` on the live board returns `"recovery_mode":true`.
`main_bridges_bringup.c:81-105` explicitly skips `lvgl_port_start()` whenever
`ctx->recovery_mode` is true, logging:
```
RECOVERY MODE: LVGL/LCD UI skipped -- the panel stays blank this boot;
recover over Wi-Fi (GET /ota) or POST /api/ota/esp/recovery_exit ...
```
The comment at that call site documents why: `ui_page_home.c` reads the
profile executor and autotune engine while building the home screen, and
recovery mode deliberately never starts those modules — LVGL was previously
made to run anyway (bench test 2026-08-22) and it panicked ~6.4 s in
(`assert failed: xQueueSemaphoreTake queue.c:1709`) trying to take a mutex
that recovery mode never created. Skipping LVGL entirely, rather than
teaching every page to tolerate a half-initialized system, is the intended
fix for that panic. So a board in recovery mode is *defined* to leave
whatever the raw-SPI boot banner drew on screen, indefinitely, with no
further paint ever issued. "kilnctl ready" staying up forever is exactly
that: expected, by-design behavior of recovery mode, not a separate display
fault.

## Evidence
- `GET /api/ota/esp/status`: `"recovery_mode":true`, `active_slot":"factory"`,
  `commit":"fc90f682"`.
- `GET /api/status`: `flush_last_us/flush_max_us/flush_count` all `0` — LVGL's
  display-flush callback has never fired since boot, matching "LVGL task
  never started" rather than "LVGL hung."
- Webcam (`capture_lcd.ps1 -Full`, `sample_lcd_region.ps1`): top-left text
  region (8,8,40x20) samples RGB(94,66,87) — brighter than an off-screen
  bezel reference sampled at RGB(8,3,20) — consistent with static bright text
  on a black background still being displayed, not a fully dark/off panel.
- `GET /api/crash_report`: `present:true, acknowledged:false,
  exc_cause_str:"LoadProhibited", exc_task:"main"` — an unacknowledged crash
  from a prior boot. (Root-causing this crash is explicitly out of scope for
  this task/session — it is owned by a separate investigation. It plausibly
  explains *why* this boot is in recovery mode, i.e. is upstream of the
  symptom, not an alternate display-path explanation.)
- Task stack margins for LVGL/display tasks could NOT be collected this
  session: `get_stack_margin()` requires the CONTROL UART link, which
  answered "no reply after all retries - link or peer is down." That
  UART-link-down condition is a separate, explicitly off-limits
  investigation in this session's scope, so it was not pursued further. Since
  `lvgl_port_start()` never ran per the code path above, there is no LVGL
  task to report margin for regardless — it does not exist this boot.

## Conclusion
Not a display/UI defect. The black screen holding "kilnCtl ready" is the
correct, documented consequence of `recovery_mode:true` skipping
`lvgl_port_start()`. The real bug to fix is why the board is in recovery mode
at all (the `LoadProhibited` panic recorded in `/api/crash_report`), which is
owned by a different investigation. No code change made here — none is
appropriate; papering over recovery mode's blank panel would defeat the
purpose documented at `main_bridges_bringup.c:94-101`.

Exit recovery mode via `POST /api/ota/esp/recovery_exit` once the panic root
cause is understood and safe to reboot past, or over `GET /ota` per the
board's own log banner.
