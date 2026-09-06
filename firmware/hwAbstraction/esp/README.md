ESP-IDF backends for the KilnFW (ESP32-S3) build, one directory per
interface (spi/ i2c/ uart/ gpio/ kv/ time/ wdt/ pwm/ sysinfo/), plus the
board descriptor (board_kiln_s3.h).

Filled starting Phase 1a (move) / Phase 1b (adapt) of
docs/HW_ABSTRACTION_PLAN.md. Empty as of Phase 0.

`gpio/hal_gpio_esp.c` (plus the shared `common/hal_esp_common.h/.c` esp_err_t->hal_status_t mapper) landed ahead of Phase 1a as bodies-only, not wired into any CMakeLists; syntax-checked by `firmware/hwAbstraction/test/compile_esp_backends.ps1`. There is no `adc/` backend here: hal_adc.h and docs/HW_ABSTRACTION_PLAN.md both describe that interface as pico-only (wrapping SaftyFW's current_task.c/current_sense.c) with no ESP consumer, so a speculative `hal_adc_esp.c` written ahead of a real caller was removed -- see the plan's hal_adc section for the interface-mismatch findings it recorded before deletion.

`uart/hal_uart_esp.c`, `spi/hal_spi_esp.c` and `i2c/hal_i2c_esp.c` landed the same way (bodies-only, not wired into any CMakeLists, syntax-checked by the same script), grounded in espInterfaces/uart_owner.c, esp_spi_owner.c and i2c_owner.c: uart preserves the read-buffered-then-zero-wait `hal_uart_recv` fix and the two send primitives' wire-complete-vs-ring-drained split; spi reproduces the owner-task/heap slot-pool/wedge-latch/DMA-sentinel machinery with no dependency on drivers/owner_slot_pool.c; i2c preserves the static per-call semaphore and worker-enforced-timeout/unbounded-caller-wait behaviors plus the bus-reset-and-retry-once recovery. hal_uart.h and hal_i2c.h have no per-owner task-sizing fields (unlike hal_spi_bus_cfg_t), so both backends hardcode stack/priority/queue-length constants rather than widening the interface -- see the INTERFACE MISMATCH comments at the top of each file.
