/* fake_gpio.c -- host fake backend for hal_gpio.h. See fake_gpio.h. */
#include "fake_gpio.h"

#include <string.h>

typedef struct {
    bool            initialized;
    hal_gpio_dir_t  dir;
    bool            level;
    hal_gpio_pull_t pull;
} fake_gpio_pin_state_t;

static fake_gpio_pin_state_t s_pins[FAKE_GPIO_NUM_PINS];
static fake_gpio_event_t     s_history[FAKE_GPIO_HISTORY_CAP];
static size_t                s_history_count;
static uint32_t              s_seq;

static bool pin_in_range(int num)
{
    return num >= 0 && num < FAKE_GPIO_NUM_PINS;
}

static void record(fake_gpio_event_kind_t kind, int pin, bool level,
                    hal_gpio_dir_t dir, hal_gpio_pull_t pull)
{
    if (s_history_count < FAKE_GPIO_HISTORY_CAP) {
        fake_gpio_event_t *e = &s_history[s_history_count++];
        e->kind = kind;
        e->pin = pin;
        e->level = level;
        e->dir = dir;
        e->pull = pull;
        e->seq = s_seq;
    }
    s_seq++;
}

void fake_gpio_reset(void)
{
    memset(s_pins, 0, sizeof(s_pins));
    memset(s_history, 0, sizeof(s_history));
    s_history_count = 0;
    s_seq = 0;
}

size_t fake_gpio_event_count(void)
{
    return s_history_count;
}

const fake_gpio_event_t *fake_gpio_event(size_t index)
{
    if (index >= s_history_count) return NULL;
    return &s_history[index];
}

bool fake_gpio_is_initialized(int pin)
{
    return pin_in_range(pin) && s_pins[pin].initialized;
}

hal_gpio_dir_t fake_gpio_current_dir(int pin)
{
    return pin_in_range(pin) ? s_pins[pin].dir : HAL_GPIO_DIR_IN;
}

bool fake_gpio_current_level(int pin)
{
    return pin_in_range(pin) && s_pins[pin].level;
}

hal_gpio_pull_t fake_gpio_current_pull(int pin)
{
    return pin_in_range(pin) ? s_pins[pin].pull : HAL_GPIO_PULL_NONE;
}

/* MUTANT-TARGET: latch-before-direction order start.
 * hal_gpio.h's contract requires the output level to be set BEFORE the pin
 * switches to output direction. The two statements/record() calls below
 * are deliberately ordered level-then-direction; test/test_host_fakes.ps1's
 * negative test swaps this block to direction-then-level and shows the
 * fake_gpio test that asserts on event order catches it. */
hal_status_t hal_gpio_init_out(int num, bool idle_level)
{
    if (!pin_in_range(num)) return HAL_INVALID_ARG;
    s_pins[num].level = idle_level;
    record(FAKE_GPIO_EV_SET_LEVEL, num, idle_level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
    s_pins[num].dir = HAL_GPIO_DIR_OUT;
    s_pins[num].initialized = true;
    record(FAKE_GPIO_EV_INIT_OUT, num, idle_level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
    return HAL_OK;
}
/* MUTANT-TARGET: latch-before-direction order end. */

hal_status_t hal_gpio_init_in(int num, hal_gpio_pull_t pull)
{
    if (!pin_in_range(num)) return HAL_INVALID_ARG;
    s_pins[num].dir = HAL_GPIO_DIR_IN;
    s_pins[num].pull = pull;
    s_pins[num].initialized = true;
    record(FAKE_GPIO_EV_INIT_IN, num, false, HAL_GPIO_DIR_IN, pull);
    return HAL_OK;
}

hal_status_t hal_gpio_set(int num, bool level)
{
    if (!pin_in_range(num)) return HAL_INVALID_ARG;
    if (!s_pins[num].initialized) return HAL_NOT_READY;
    if (s_pins[num].dir != HAL_GPIO_DIR_OUT) return HAL_INVALID_ARG; /* wrong direction */
    s_pins[num].level = level;
    record(FAKE_GPIO_EV_SET_LEVEL, num, level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
    return HAL_OK;
}

bool hal_gpio_get(int num)
{
    /* No hal_status_t return on this signature -- an out-of-range or
     * never-initialized pin reads as false rather than reporting an error.
     * Documented limitation; see the report for this task. */
    if (!pin_in_range(num) || !s_pins[num].initialized) return false;
    return s_pins[num].level;
}

hal_status_t hal_gpio_set_direction(int num, hal_gpio_dir_t dir)
{
    if (!pin_in_range(num)) return HAL_INVALID_ARG;
    s_pins[num].dir = dir;
    s_pins[num].initialized = true;
    record(FAKE_GPIO_EV_SET_DIRECTION, num, s_pins[num].level, dir, s_pins[num].pull);
    return HAL_OK;
}

hal_status_t hal_gpio_set_pull(int num, hal_gpio_pull_t pull)
{
    if (!pin_in_range(num)) return HAL_INVALID_ARG;
    s_pins[num].pull = pull;
    record(FAKE_GPIO_EV_SET_PULL, num, s_pins[num].level, s_pins[num].dir, pull);
    return HAL_OK;
}
