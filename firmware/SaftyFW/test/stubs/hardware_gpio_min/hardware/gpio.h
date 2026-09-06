/* gpio.h -- minimal host stub for pico-sdk's hardware/gpio.h.
 *
 * Scope: only what max31856.c's max31856_init() calls directly for the
 * CS/~FAULT GPIOs (gpio_init/gpio_put/gpio_set_dir/gpio_pull_up), which are
 * OUTSIDE this HAL Phase 1b pass's scope (only the SPI transport moved to
 * hal_spi.h -- see max31856.c's own top-of-file comment). These calls are
 * host-inert no-ops recording nothing; test_max31856_hal_spi.c's host test
 * exercises max31856_bus_init()/_configure()/_read() against fake_spi.c, not
 * GPIO behavior, so a real/fake GPIO model is not needed here -- unlike
 * fake_gpio.c (hwAbstraction/host/), which backs relay_owner.c's actual
 * hal_gpio *behavioral* test.
 */
#ifndef SAFTYFW_HOST_STUB_HARDWARE_GPIO_H
#define SAFTYFW_HOST_STUB_HARDWARE_GPIO_H

#define GPIO_OUT 1
#define GPIO_IN  0

static inline void gpio_init(unsigned pin) { (void)pin; }
static inline void gpio_put(unsigned pin, int value) { (void)pin; (void)value; }
static inline void gpio_set_dir(unsigned pin, int dir) { (void)pin; (void)dir; }
static inline void gpio_pull_up(unsigned pin) { (void)pin; }
/* Host tests never assert on ~FAULT-pin behavior for this consumer (that is
 * max31856_fault_pin_policy.h's own host-tested unit, exercised by
 * test_max31856_fault_pin_policy.c against real inputs) -- fixed high
 * (not asserted) is sufficient here. */
static inline int gpio_get(unsigned pin) { (void)pin; return 1; }

#endif /* SAFTYFW_HOST_STUB_HARDWARE_GPIO_H */
