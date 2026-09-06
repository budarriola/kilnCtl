// test_relay_owner_gpio_init.c -- HAL Phase 1b (docs/HW_ABSTRACTION_PLAN.md
// "hal_gpio" section): relay_owner.c is now a hal_gpio client, and this is
// the host-testable init-order property Phase 2 promised (fake_gpio.h's
// header comment: "order-checkable ... latch-before-direction sequencing").
//
// relay_owner_start() now re-asserts the fail-safe default itself, via
// hal_gpio_init_out(SAFTYFW_PIN_RELAY, false), before creating the task (see
// relay_owner.c/.h's own comments on that call). This test drives
// relay_owner_start() for real against firmware/hwAbstraction/host/fake_gpio.c
// and checks, from fake_gpio's own event history, that:
//   1. the relay pin ends up initialized, direction OUT, level LOW
//      (de-energized at init)
//   2. the two hal_gpio_init_out() events for that pin -- SET_LEVEL then
//      INIT_OUT (fake_gpio.c's own event pair for one init_out() call) --
//      appear in that order, i.e. latch-before-direction, not the reverse
//
// relay_owner_task()'s for(;;) loop is never entered here -- there is no
// FreeRTOS scheduler on host -- see test_relay_owner_gpio_init_stubs.c for
// the minimal xQueueCreate/xTaskCreate/watchdog_task_checkin bodies that let
// relay_owner_start()'s real pre-task-creation code run and let the
// translation unit link.
#include "test_common.h"

#include "fake_gpio.h"
#include "hal_gpio.h"

#include "../src/board_pins.h"
#include "../src/tasks/relay_owner.h"

static void test_relay_owner_start_energizes_nothing_and_latches_before_direction(void)
{
    TEST_SECTION("relay_owner_start() -- fail-safe default at init: de-energized, "
                 "latch-before-direction (HAL Phase 1b)");

    fake_gpio_reset();

    bool ok = relay_owner_start();
    TEST_CHECK(ok, "relay_owner_start() succeeds against the host stubs");

    TEST_CHECK(fake_gpio_is_initialized(SAFTYFW_PIN_RELAY),
               "the relay pin is initialized after relay_owner_start()");
    TEST_CHECK(fake_gpio_current_dir(SAFTYFW_PIN_RELAY) == HAL_GPIO_DIR_OUT,
               "the relay pin ends up in OUT direction");
    TEST_CHECK(fake_gpio_current_level(SAFTYFW_PIN_RELAY) == false,
               "the relay pin is de-energized (LOW) immediately after init -- "
               "a relay must never glitch on at init");

    // fake_gpio.c's hal_gpio_init_out() records exactly two events for one
    // call, in this order: SET_LEVEL (the latch, level applied BEFORE
    // direction changes) then INIT_OUT (direction switched to OUT). Both
    // must be present, for this pin, in that order -- reversing them is
    // exactly the glitch hal_gpio.h's latch-before-direction contract
    // exists to prevent. relay_owner_start() calls hal_gpio_init_out()
    // before doing anything else, so these are the first two events in a
    // freshly-reset history.
    TEST_CHECK(fake_gpio_event_count() >= 2,
               "at least two fake_gpio events were recorded (the init_out() pair)");

    const fake_gpio_event_t *e0 = fake_gpio_event(0);
    const fake_gpio_event_t *e1 = fake_gpio_event(1);
    TEST_CHECK(e0 != NULL && e1 != NULL, "both expected events exist");
    if (e0 && e1) {
        TEST_CHECK(e0->pin == SAFTYFW_PIN_RELAY && e1->pin == SAFTYFW_PIN_RELAY,
                   "both events are for the relay pin (GPIO6)");
        TEST_CHECK(e0->kind == FAKE_GPIO_EV_SET_LEVEL,
                   "first event is SET_LEVEL (the latch) -- if this fails, "
                   "relay_owner is no longer driving the level before switching "
                   "direction, and a relay could glitch on at init");
        TEST_CHECK(e0->level == false,
                   "the latched level is LOW (de-energized), not the caller-supplied "
                   "'true' the ENERGIZE case is the only line allowed to send");
        TEST_CHECK(e1->kind == FAKE_GPIO_EV_INIT_OUT,
                   "second event is INIT_OUT (direction switched to OUT), after the latch");
    }
}

void run_test_relay_owner_gpio_init(void)
{
    test_relay_owner_start_energizes_nothing_and_latches_before_direction();
}
