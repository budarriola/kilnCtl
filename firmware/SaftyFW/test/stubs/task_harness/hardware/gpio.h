/* gpio.h -- host stub of pico-sdk hardware/gpio.h for the task-loop
 * harness. Unlike stubs\hardware_gpio_min\ (inert inline no-ops) the pin
 * level read by gpio_get() is controllable and the ~DRDY IRQ callback that
 * thermo_task_fn() registers is captured, so a test can fire it. */
#ifndef SAFTYFW_TASK_HARNESS_HARDWARE_GPIO_H
#define SAFTYFW_TASK_HARNESS_HARDWARE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

#define GPIO_OUT 1
#define GPIO_IN  0
#define GPIO_IRQ_EDGE_FALL 0x4u

typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t event_mask);

void gpio_init(uint pin);
void gpio_put(uint pin, bool value);
void gpio_set_dir(uint pin, bool out);
void gpio_pull_up(uint pin);
bool gpio_get(uint pin);
void gpio_set_irq_enabled_with_callback(uint pin, uint32_t events, bool enabled,
                                         gpio_irq_callback_t callback);

#endif
