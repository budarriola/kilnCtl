# Review: OR-03 rollback reboot on esp_timer (eb44544cb), 2026-10-18

Scope: `ota_http_esp.c` rollback reboot moved from a 4096 B on-demand task to a one-shot
esp_timer (500 ms), its tests (`test_ota_http.c`, `stubs/esp_timer.h`), and the check/budget rows. Static review, no board.

Counts: HIGH 0, MED 0, LOW 3, INFO 4. Prefix ET.

## Findings
- **ET-01 LOW.** The callback comment says it "never waits on a lock or queue". That is wrong:
  `safety_link_send_announce_reboot()` -> `uart_protocol_send_broadcast()` takes `proto->tx_lock` with
  `portMAX_DELAY`, and `flash_xip_reader_enter()` takes the flash XIP recursive mutex with `portMAX_DELAY`
  (CONFIG_SPIRAM_XIP_FROM_PSRAM=y). While it waits, every esp_timer-task callback stalls: `lv_tick_timer_cb`,
  `dram_watch` sample, Wi-Fi rescan/AP-fallback, IDF Wi-Fi internal timers. The waits are bounded by the
  holders (one UART frame; one LittleFS/NVS erase or write). No watchdog or control-loop timer runs on
  esp_timer, and a blocked task yields, so the idle-task WDT is not at risk. The board reboots right
  afterwards, so the stall does no harm. Fix: correct the comment ("bounded waits on tx_lock and the
  flash XIP mutex").
- **ET-02 LOW.** The stack headroom has not been measured. The callback now runs on the esp_timer task stack
  (sdkconfig.defaults does not set it, so it is the IDF default `CONFIG_ESP_TIMER_TASK_STACK_SIZE` 3584 B, below
  the old 4096 B). The chain is ESP_LOG formatting, the announce encode, the otadata read/CRC/erase/write,
  then `esp_restart()` shutdown handlers (Wi-Fi stop) on that same stack. The commit also removed the
  `stack_margin` slot, so no telemetry covers this path. An overflow after the otadata write still ends in a
  reboot into the previous image, but with a panic reset reason (boot_guard/S6b accounting). Fix: one bench
  rollback while reading `uxTaskGetStackHighWaterMark` of the esp_timer task, or register the esp_timer
  task in stack_margin.
- **ET-03 LOW.** The tests cover less than the change. The stub `esp_timer_create` always succeeds, so the
  create-failure branch (handle reset to NULL, 500, claim released) is never exercised. The tests only force
  `esp_timer_start_once` to fail. No test arms the timer twice (for example a retry after a failed callback).
  The callback is called directly, which is fine for OP-02.
- **ET-04 INFO.** The OTA claim is released exactly once on each path. Arm failure: the handler calls
  `ota_http_update_end()` once and returns 500. Reboot call failure: the callback releases it once. Success:
  the claim stays held until reset, as intended.
- **ET-05 INFO.** Repeat requests while the timer is armed are refused at `ota_http_update_try_begin()`, because
  the claim is held, so `esp_timer_start_once` is never called on a running timer (which would be
  ESP_ERR_INVALID_STATE). After a failed callback the claim is free and the timer has expired, so re-arming works.
- **ET-06 INFO.** The flash_xip bracket is balanced and unchanged in substance. The mutex is recursive and
  task-owned, and the esp_timer task owns it for the whole bracket. Running flash ops from the esp_timer task
  is allowed under IDF. The esp_timer task stack is internal RAM.
- **ET-07 INFO.** OR-03 is only partly fixed. `ota_pico_rollback` is still a 4096 B on-demand task (KNOWN_ISSUES
  already says so).
