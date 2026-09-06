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
