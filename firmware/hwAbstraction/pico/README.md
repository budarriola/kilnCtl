pico-sdk backends for the SaftyFW (RP2040) build: spi/ uart/ gpio/ adc/
flash/ scratch/ wdt/ time/, plus the board descriptor
(board_safety_rp2040.h).

Filled starting Phase 1a (move) / Phase 1b (adapt) of
docs/HW_ABSTRACTION_PLAN.md. Empty as of Phase 0.

`gpio/hal_gpio_pico.c` and `adc/hal_adc_pico.c` landed ahead of Phase 1a as
bodies-only, not wired into any CMakeLists; syntax-checked by
`firmware/hwAbstraction/test/compile_pico_backends.ps1`. Both are validated
against real SaftyFW consumers (tasks/relay_owner.c, tasks/current_task.c,
current_sense.c, spi_owner.c, max31856.c, tasks/discrete_task.c,
tasks/watchdog_task.c) -- no interface mismatch found for either.

`uart/hal_uart_pico.c` and `time/hal_time_pico.c` landed the same way
(bodies-only, syntax-checked by the same script), grounded against real
SaftyFW consumers (tasks/uart_owner.c, pico-sdk pico_time): no mismatch for
uart/time beyond uart's single-fixed-instance/no-rx-error-counter/
no-restart notes. `hal_uart_recv_blocking()` (polls `uart_owner_rx_read()`
via `hal_time_delay_ms(1)` until a byte arrives or timeout) and
`hal_uart_get_task_handle()` (always NULL) were added the same way once
hal_uart.h grew those two functions plus the queue_len/task_priority/
stack_depth/core_id cfg fields: uart_owner.c is IRQ-driven with no FreeRTOS
task at all, so hal_uart_init() requires the three task-sizing fields to be
0 and rejects any nonzero value (core_id is exempt -- hal_uart_cfg_t's own
doc comment says pico/host backends ignore it outright).

`flash/hal_flash_pico.c` landed the same way (bodies-only, syntax-checked by
the same script), grounded against config_store_flash.c's real XIP-mapped
read / conditional flash_range_erase()+flash_range_program() / 1000ms
flash_safe_execute() call: two interface mismatches noted in its own header
comment (PICO_FLASH_SIZE_BYTES is compiled-in, not runtime-queryable; no
pico-sdk predicate backs hal_flash_write_safe_here(), so it is advisory-only
and always returns true). config_store_flash.c's own rebase onto this header
(Phase 3 item 2) has not happened yet.

There is deliberately no `kv/hal_kv_pico.c` -- hal_kv.h explicitly excludes
pico (config_store is not a key/value store: one fixed 512 B record,
seq/CRC/ARMED-gated, no partitions/string fields/scoped erase). An earlier
attempt at a narrow mapping onto config_store's real record read/write was
removed per review: it pulled a SaftyFW header (config_store.h) across the
one-way hwAbstraction boundary and invented a third commit-durability
model alongside NVS's and fake_kv's. config_store's real home is
`hal_flash` (pico backend for config_store_flash.c) -- see
docs/HW_ABSTRACTION_PLAN.md's `hal_flash` section, which now carries the
ARMED-interlock/seq-CRC-log/format-version-REFUSE design that file's header
comment worked out, as prose for whoever writes that backend.
