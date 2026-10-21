# REVIEW_ET02BOOT_2026_10_19

Scope: ET-02 boot-time esp_timer stack_margin registration, b94bf3516 + 8c4c7d088 (promoted in c86ef946c).
Files: `App/main_network_http.c`, `App/drivers/http/ota_http_esp.c`, `App/test/test_ota_http.c`,
`tools/check_stack_margin_registration.ps1`, `tools/check_stack_task_table_consistency.ps1`.

HIGH 0, MED 0, LOW 3, INFO 4.

## Findings

- **ER-01** LOW: `sdkconfig.defaults` ET-02 comment (line ~141) still says "Keep equal to
  OTA_ESP_TIMER_TASK_STACK_BYTES in ota_http_esp.c"; that macro was removed. The value is now a bare
  literal `4608` in `main_network_http.c`. Fix: point the comment at main_network_http.c.
- **ER-02** LOW: nothing ties the registered size to `CONFIG_ESP_TIMER_TASK_STACK_SIZE`. The literal
  4608 is hardcoded twice (sdkconfig.defaults, main_network_http.c); the host test only checks the
  literal "4608" appears near the call, so changing sdkconfig alone passes every test and check and
  the reported margin becomes wrong. Fix: pass `CONFIG_ESP_TIMER_TASK_STACK_SIZE` (sdkconfig.h) in the
  firmware, or have a check compare the literal to sdkconfig.defaults.
- **ER-03** LOW: a NULL `xTaskGetHandle("esp_timer")` (IDF rename, or timer task not yet created) is
  silent: no log, row just missing. `check_stack_margin_registration.ps1` only proves the call site
  exists. Fix: ESP_LOGW on NULL so a bench log shows the gap.
- **ER-04** INFO: handle lifetime is fine. The slot is a function-scope `static`, stack_margin keeps the
  pointer; the esp_timer task never exits, so no `stack_margin_task_exit` is needed. Bringup has no
  early return before the registration (only return in the file is line 308, outside bringup).
- **ER-05** INFO: duplicate registration not possible: bringup runs once (main.c:266), and the
  registry is idempotent by (name, slot) anyway. The old rollback-path site is gone, so no name/slot
  collision. Registry capacity 48 vs ~43 distinct literal names; the cap check in
  check_stack_margin_registration.ps1 covers it.
- **ER-06** INFO: host test is a source scan: catches the call moving back into ota_http_esp.c or the
  boot call being deleted (any `stack_margin_register(`/`xTaskGetHandle(` in ota_http_esp.c fails it),
  but not ER-02/ER-03. Acceptable for an IDF-owned task with no host-runnable body.
- **ER-07** INFO: stale nearby comments: `dram_watch.h:21` and `dram_watch.c:25` still say the esp_timer
  stack is 3584 B (now 4608). Doc-only.

Bench: the esp_timer row's high-water measurement remains BENCH-PENDING (sdkconfig.defaults note).
