ESP-IDF backends for the KilnFW (ESP32-S3) build, one directory per
interface (spi/ i2c/ uart/ gpio/ kv/ time/ wdt/ pwm/ sysinfo/), plus the
board descriptor (board_kiln_s3.h).

Filled starting Phase 1a (move) / Phase 1b (adapt) of
docs/HW_ABSTRACTION_PLAN.md. Empty as of Phase 0.

`gpio/hal_gpio_esp.c` and `adc/hal_adc_esp.c` (plus the shared `common/hal_esp_common.h/.c` esp_err_t->hal_status_t mapper) landed ahead of Phase 1a as bodies-only, not wired into any CMakeLists; syntax-checked by `firmware/hwAbstraction/test/compile_esp_backends.ps1`. hal_adc_esp.c is speculative -- see its top-of-file "INTERFACE MISMATCH" comment: hal_adc.h and the plan both describe this interface as pico-only (wrapping SaftyFW's current_task.c/current_sense.c), so there is no real ESP consumer to validate against.
