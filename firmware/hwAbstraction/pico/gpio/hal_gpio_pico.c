/* hal_gpio_pico.c -- pico-sdk backend for interface/hal_gpio.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against pico-sdk 2.x
 * (hardware_gpio/include/hardware/gpio.h). Not wired into any CMakeLists yet
 * -- see firmware/hwAbstraction/test/compile_pico_backends.ps1 for the
 * syntax-only compile check that stands in for that until Phase 1a's real
 * move lands.
 *
 * Clients this backend must serve unchanged (docs/HW_ABSTRACTION.md
 * "hal_gpio" section, validated against firmware/SaftyFW/src): the relay
 * output pin (tasks/relay_owner.c writes it, main.c:151-153 brings it up),
 * SPI0 CS0 (spi_owner.c), MAX31856 CS + fault-input pins (max31856.c),
 * thermocouple DRDY input (tasks/thermo_task.c), E-stop and main-fault
 * inputs (tasks/discrete_task.c), and the heartbeat LED output
 * (tasks/watchdog_task.c). gpio_set_function() pin-mux calls for SPI0 and
 * UART0/UART1 (spi_owner.c, tasks/console_uart.c, tasks/uart_owner.c) stay
 * raw, outside this interface -- hal_gpio.h has no pin-mux concept and none
 * of the above needs one beyond the peripheral's own bring-up.
 *
 * Latch-before-direction (hal_gpio.h contract): pico-sdk's gpio_put()
 * writes the output-data register unconditionally regardless of the pin's
 * current direction (hardware_gpio/gpio.c -> sio_hw->gpio_set/clr, which is
 * independent of the GPIO's OE bit), and gpio_set_dir() only touches the
 * OE bit, never the output-data register. So calling gpio_put() BEFORE
 * gpio_set_dir(..., GPIO_OUT) is glitch-free on this backend, matching
 * every real SaftyFW call site already in the tree today (main.c:151-153
 * does put-then-set_dir; see docs/HW_ABSTRACTION.md's "Init ordering
 * audit" -- bootloader/main.c's reversed order is a separate, already
 * flagged, out-of-scope bug). hal_gpio_init_out() below relies on exactly
 * this ordering.
 *
 * All raw IRQ registration (pico's DRDY dispatcher) intentionally stays
 * OUTSIDE this interface per the "clean-room; no IRQ surface in v1"
 * decision in hal_gpio.h and the plan; this backend does not provide any
 * ISR path.
 */
#include "hal_gpio.h"

#include "hardware/gpio.h"

static void hal_gpio_pico_apply_pull(int num, hal_gpio_pull_t pull) {
    switch (pull) {
        case HAL_GPIO_PULL_UP:
            gpio_pull_up((uint)num);
            break;
        case HAL_GPIO_PULL_DOWN:
            gpio_pull_down((uint)num);
            break;
        case HAL_GPIO_PULL_NONE:
        default:
            gpio_disable_pulls((uint)num);
            break;
    }
}

hal_status_t hal_gpio_init_out(int num, bool idle_level) {
    /* gpio_init() resets the pin to a known (input, no-pull, SIO-function)
     * state; matches every real SaftyFW call site (main.c, spi_owner.c,
     * max31856.c, watchdog_task.c all call gpio_init() first). */
    gpio_init((uint)num);
    /* Level first -- see latch-before-direction note above. */
    gpio_put((uint)num, idle_level);
    gpio_set_dir((uint)num, GPIO_OUT);
    return HAL_OK;
}

hal_status_t hal_gpio_init_in(int num, hal_gpio_pull_t pull) {
    gpio_init((uint)num);
    gpio_set_dir((uint)num, GPIO_IN);
    hal_gpio_pico_apply_pull(num, pull);
    return HAL_OK;
}

hal_status_t hal_gpio_set(int num, bool level) {
    gpio_put((uint)num, level);
    return HAL_OK;
}

bool hal_gpio_get(int num) {
    return gpio_get((uint)num);
}

/* gpio_probe.c-equivalent runtime pin scanning only -- see contract note in
 * hal_gpio.h. */
hal_status_t hal_gpio_set_direction(int num, hal_gpio_dir_t dir) {
    gpio_set_dir((uint)num, dir == HAL_GPIO_DIR_OUT);
    return HAL_OK;
}

hal_status_t hal_gpio_set_pull(int num, hal_gpio_pull_t pull) {
    hal_gpio_pico_apply_pull(num, pull);
    return HAL_OK;
}

/* Interface-mismatch notes (docs/HW_ABSTRACTION.md asks these to be
 * reported, not silently papered over by widening hal_gpio.h):
 *
 * 1. Pin-mux selection (gpio_set_function(), GPIO_FUNC_SPI/GPIO_FUNC_UART)
 *    has no representation in hal_gpio.h -- spi_owner.c, console_uart.c and
 *    uart_owner.c all call it directly for their peripheral pins and must
 *    keep doing so; this backend does not attempt to cover it.
 * 2. hal_gpio_init_in()'s hal_gpio_pull_t cannot express both pull-up and
 *    pull-down enabled simultaneously (pico-sdk's gpio_pull_up() +
 *    gpio_pull_down() can be called together); no current SaftyFW consumer
 *    needs that combination.
 */
