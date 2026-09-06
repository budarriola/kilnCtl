ESP-IDF backends for the KilnFW (ESP32-S3) build, one directory per
interface (spi/ i2c/ uart/ gpio/ kv/ time/ wdt/ pwm/ sysinfo/), plus the
board descriptor (board_kiln_s3.h).

Filled starting Phase 1a (move) / Phase 1b (adapt) of
docs/HW_ABSTRACTION_PLAN.md. Empty as of Phase 0.

`gpio/hal_gpio_esp.c` (plus the shared `common/hal_esp_common.h/.c` esp_err_t->hal_status_t mapper) landed ahead of Phase 1a as bodies-only, not wired into any CMakeLists; syntax-checked by `firmware/hwAbstraction/test/compile_esp_backends.ps1`. There is no `adc/` backend here: hal_adc.h and docs/HW_ABSTRACTION_PLAN.md both describe that interface as pico-only (wrapping SaftyFW's current_task.c/current_sense.c) with no ESP consumer, so a speculative `hal_adc_esp.c` written ahead of a real caller was removed -- see the plan's hal_adc section for the interface-mismatch findings it recorded before deletion.
