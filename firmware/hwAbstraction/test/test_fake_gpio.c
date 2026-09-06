/* test_fake_gpio.c -- standalone MSVC host test for hwAbstraction/host/fake_gpio.c.
 * Compiled and run by test_host_fakes.ps1. Exercises every hal_gpio.h
 * function including error paths, plus the negative-test target: event[0]
 * must be SET_LEVEL, not INIT_OUT (latch-before-direction).
 */
#include <stdio.h>

#include "fake_gpio.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void)
{
    fake_gpio_reset();

    /* --- init_out: invalid pin numbers -> HAL_INVALID_ARG --- */
    CHECK(hal_gpio_init_out(-1, false) == HAL_INVALID_ARG);
    CHECK(hal_gpio_init_out(FAKE_GPIO_NUM_PINS, false) == HAL_INVALID_ARG);
    CHECK(fake_gpio_is_initialized(-1) == false);

    /* --- init_out: valid pin, order and current state --- */
    fake_gpio_reset();
    CHECK(hal_gpio_init_out(5, true) == HAL_OK);
    CHECK(fake_gpio_is_initialized(5) == true);
    CHECK(fake_gpio_current_dir(5) == HAL_GPIO_DIR_OUT);
    CHECK(fake_gpio_current_level(5) == true);
    CHECK(hal_gpio_get(5) == true);

    /* Latch-before-direction: the level-set event must be recorded before
     * the init_out event. This is the assertion the negative test breaks. */
    CHECK(fake_gpio_event_count() == 2);
    if (fake_gpio_event_count() == 2) {
        const fake_gpio_event_t *e0 = fake_gpio_event(0);
        const fake_gpio_event_t *e1 = fake_gpio_event(1);
        CHECK(e0 != NULL && e0->kind == FAKE_GPIO_EV_SET_LEVEL);
        CHECK(e1 != NULL && e1->kind == FAKE_GPIO_EV_INIT_OUT);
    }
    CHECK(fake_gpio_event(999) == NULL);

    /* --- init_in --- */
    fake_gpio_reset();
    CHECK(hal_gpio_init_in(-1, HAL_GPIO_PULL_UP) == HAL_INVALID_ARG);
    CHECK(hal_gpio_init_in(7, HAL_GPIO_PULL_UP) == HAL_OK);
    CHECK(fake_gpio_current_dir(7) == HAL_GPIO_DIR_IN);
    CHECK(fake_gpio_current_pull(7) == HAL_GPIO_PULL_UP);

    /* --- set: uninitialized handle (pin) -> HAL_NOT_READY --- */
    fake_gpio_reset();
    CHECK(hal_gpio_set(3, true) == HAL_NOT_READY);

    /* --- set: wrong direction -> HAL_INVALID_ARG --- */
    fake_gpio_reset();
    CHECK(hal_gpio_init_in(3, HAL_GPIO_PULL_NONE) == HAL_OK);
    CHECK(hal_gpio_set(3, true) == HAL_INVALID_ARG);

    /* --- set: out-of-range pin -> HAL_INVALID_ARG --- */
    CHECK(hal_gpio_set(-1, true) == HAL_INVALID_ARG);
    CHECK(hal_gpio_set(FAKE_GPIO_NUM_PINS, true) == HAL_INVALID_ARG);

    /* --- set: valid OUT pin succeeds and updates level --- */
    fake_gpio_reset();
    CHECK(hal_gpio_init_out(10, false) == HAL_OK);
    CHECK(hal_gpio_set(10, true) == HAL_OK);
    CHECK(hal_gpio_get(10) == true);
    CHECK(hal_gpio_set(10, false) == HAL_OK);
    CHECK(hal_gpio_get(10) == false);

    /* --- get: uninitialized / out-of-range -> false (no status on this
     * signature) --- */
    fake_gpio_reset();
    CHECK(hal_gpio_get(-1) == false);
    CHECK(hal_gpio_get(FAKE_GPIO_NUM_PINS) == false);
    CHECK(hal_gpio_get(20) == false); /* never touched */

    /* --- set_direction / set_pull: gpio_probe escape hatch --- */
    fake_gpio_reset();
    CHECK(hal_gpio_set_direction(-1, HAL_GPIO_DIR_OUT) == HAL_INVALID_ARG);
    CHECK(hal_gpio_set_direction(4, HAL_GPIO_DIR_IN) == HAL_OK);
    CHECK(fake_gpio_current_dir(4) == HAL_GPIO_DIR_IN);
    CHECK(fake_gpio_is_initialized(4) == true);
    CHECK(hal_gpio_set_pull(-1, HAL_GPIO_PULL_DOWN) == HAL_INVALID_ARG);
    CHECK(hal_gpio_set_pull(4, HAL_GPIO_PULL_DOWN) == HAL_OK);
    CHECK(fake_gpio_current_pull(4) == HAL_GPIO_PULL_DOWN);

    /* --- history stays bounded (fixed-cap log, does not wrap) --- */
    fake_gpio_reset();
    for (int i = 0; i < FAKE_GPIO_HISTORY_CAP + 10; i++) {
        (void)hal_gpio_set_pull(0, HAL_GPIO_PULL_NONE);
    }
    CHECK(fake_gpio_event_count() == FAKE_GPIO_HISTORY_CAP);

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
