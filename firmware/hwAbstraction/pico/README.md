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
no-restart notes.

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
