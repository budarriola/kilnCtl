/* fake_gpio.h -- host fake backend for hal_gpio.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "Host fakes (Phase 2 specs)" -- fake_gpio
 * (must): per-pin level AND direction history (order-checkable CS and
 * latch-before-direction sequencing), link-shared state instead of the old
 * static-per-TU counters.
 *
 * Deterministic, single-threaded, no OS primitives. State and the event
 * history are process-global (link-shared, matching the plan's wording) --
 * call fake_gpio_reset() between test cases.
 */
#ifndef KILNCTL_FAKE_GPIO_H
#define KILNCTL_FAKE_GPIO_H

#include <stddef.h>
#include <stdint.h>

#include "hal_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_GPIO_NUM_PINS   64
#define FAKE_GPIO_HISTORY_CAP 512

typedef enum {
    FAKE_GPIO_EV_SET_LEVEL = 0,
    FAKE_GPIO_EV_INIT_OUT,
    FAKE_GPIO_EV_INIT_IN,
    FAKE_GPIO_EV_SET_DIRECTION,
    FAKE_GPIO_EV_SET_PULL,
} fake_gpio_event_kind_t;

typedef struct {
    fake_gpio_event_kind_t kind;
    int             pin;
    bool            level;
    hal_gpio_dir_t  dir;
    hal_gpio_pull_t pull;
    uint32_t        seq;
} fake_gpio_event_t;

/* Clears all pin state and the event history. Call between test cases. */
void fake_gpio_reset(void);

/* Ordered event history, oldest first. index >= fake_gpio_event_count()
 * returns NULL. The history is a fixed-capacity log (FAKE_GPIO_HISTORY_CAP)
 * that stops recording once full rather than wrapping -- host test runs are
 * bounded and a silent wrap would corrupt order-checking assertions. */
size_t fake_gpio_event_count(void);
const fake_gpio_event_t *fake_gpio_event(size_t index);

/* Current-state accessors, for tests that only care about the end state. */
bool            fake_gpio_is_initialized(int pin);
hal_gpio_dir_t  fake_gpio_current_dir(int pin);
bool            fake_gpio_current_level(int pin);
hal_gpio_pull_t fake_gpio_current_pull(int pin);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_GPIO_H */
