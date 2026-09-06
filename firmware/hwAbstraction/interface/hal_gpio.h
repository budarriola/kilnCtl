/* hal_gpio.h -- portable GPIO interface. Clean-room; no IRQ surface in v1.
 *
 * See docs/HW_ABSTRACTION.md "hal_gpio -- clean-room; no IRQ surface
 * in v1". Exactly two IRQ registrations exist today (pico DRDY via a
 * shared per-core dispatcher, ESP SX1509 ~INT via gpio_isr_handler_add) and
 * both stay raw, outside this interface, until a second pico consumer
 * forces a hal_gpio_irq_attach() addition.
 *
 * Threading/ownership contract:
 *  - No implicit locking: hal_gpio_set/get operate directly on the pin;
 *    callers that need atomicity across multiple pins (e.g. bit-banged CS
 *    sequences) must serialize themselves, same as today.
 *  - Latch-before-direction: hal_gpio_init_out(num, idle_level) MUST set
 *    the output level BEFORE switching the pin to output direction. This
 *    is a correctness requirement, not a style preference -- three ESP
 *    sites do direction-then-level today and have a brief glitch window at
 *    boot (panel_spi_bringup.c CS/reset, MAX31856.c CS); migrating them to
 *    this contract is a deliberate, documented behavior fix, not a silent
 *    one. (On this ESP-IDF version gpio_set_direction never touches the
 *    output register, so level-then-config is glitch-free; verify the same
 *    holds for any future backend before relying on it.)
 *  - hal_gpio_set_direction/set_pull are a narrow escape hatch for
 *    gpio_probe.c's runtime pin scanning and for adjusting one property of
 *    a pin another peripheral owns (safety_link.c's UART RX pull-up) --
 *    ordinary drivers use init_out/init_in and must not reach for these.
 */
#ifndef KILNCTL_HAL_GPIO_H
#define KILNCTL_HAL_GPIO_H

#include <stdbool.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_GPIO_PULL_NONE = 0,
    HAL_GPIO_PULL_UP,
    HAL_GPIO_PULL_DOWN,
} hal_gpio_pull_t;

typedef enum {
    HAL_GPIO_DIR_IN = 0,
    HAL_GPIO_DIR_OUT,
} hal_gpio_dir_t;

/* Sets idle_level BEFORE switching direction -- see latch-before-direction
 * contract above. */
hal_status_t hal_gpio_init_out(int num, bool idle_level);
hal_status_t hal_gpio_init_in(int num, hal_gpio_pull_t pull);

hal_status_t hal_gpio_set(int num, bool level);
bool         hal_gpio_get(int num);

/* gpio_probe.c only -- see contract note above. */
hal_status_t hal_gpio_set_direction(int num, hal_gpio_dir_t dir);
hal_status_t hal_gpio_set_pull(int num, hal_gpio_pull_t pull);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_GPIO_H */
