// test_relay_owner_gpio_init.c -- HAL Phase 1b (docs/HW_ABSTRACTION_PLAN.md
// "hal_gpio" section): relay_owner.c is now a hal_gpio client, and this is
// the host-testable init-order property Phase 2 promised (fake_gpio.h's
// header comment: "order-checkable ... latch-before-direction sequencing").
//
// relay_owner_start() now re-asserts the fail-safe default itself via
// hal_gpio_set(SAFTYFW_PIN_RELAY, false) followed by
// hal_gpio_set_direction(SAFTYFW_PIN_RELAY, HAL_GPIO_DIR_OUT) -- NOT
// hal_gpio_init_out() (see relay_owner.c's own comment on why: init_out()
// on the pico backend calls gpio_init() first, which resets the pin to
// INPUT before re-latching it, opening a high-Z window on the relay gate
// that main.c's original boot-time put-then-set_dir sequence never had).
//
// Known host/hardware mirror gap this test documents rather than hides:
// fake_gpio.c's hal_gpio_set() refuses to apply a level on a pin that
// isn't yet marked .initialized (returns HAL_NOT_READY, no SET_LEVEL event
// recorded) -- a check the real pico backend's gpio_put() does not make,
// since it writes the output register unconditionally regardless of
// direction/init state. On a freshly-reset fake_gpio (this test's starting
// state) the pin is never initialized, so relay_owner_start()'s
// hal_gpio_set() call is a no-op on host; hal_gpio_set_direction() then
// marks the pin initialized (dir OUT) and the pin's level field is left at
// its zero-initialized default (false), which happens to be the desired
// de-energized value. On real hardware the pin is already OUT+LOW from
// main.c's boot-time init, so hal_gpio_set(pin, false) there is a genuine
// (redundant but harmless) latch, not a no-op. This test therefore checks
// end state and the SET_DIRECTION event, not a SET_LEVEL-then-SET_DIRECTION
// event pair -- fake_gpio.c would need an "already initialized by a raw,
// pre-HAL boot call" seam to model the real ordering faithfully, which does
// not exist today.
//
// relay_owner_task()'s for(;;) loop is never entered here -- there is no
// FreeRTOS scheduler on host -- see test_relay_owner_gpio_init_stubs.c for
// the minimal xQueueCreate/xTaskCreate/watchdog_task_checkin bodies that let
// relay_owner_start()'s real pre-task-creation code run and let the
// translation unit link.
#include "test_common.h"

#include "fake_gpio.h"
#include "hal_gpio.h"

#include "../src/board/board_pins.h"
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

    // See the file header for why this is a SET_DIRECTION event, not a
    // SET_LEVEL-then-INIT_OUT pair, on a freshly-reset host fake: fake_gpio's
    // hal_gpio_set() refuses an un-initialized pin, so relay_owner_start()'s
    // hal_gpio_set() call records nothing here and hal_gpio_set_direction()
    // is the event that actually initializes the pin.
    TEST_CHECK(fake_gpio_event_count() >= 1,
               "at least one fake_gpio event was recorded (SET_DIRECTION)");

    const fake_gpio_event_t *e0 = fake_gpio_event(0);
    TEST_CHECK(e0 != NULL, "the expected event exists");
    if (e0) {
        TEST_CHECK(e0->pin == SAFTYFW_PIN_RELAY, "the event is for the relay pin (GPIO6)");
        TEST_CHECK(e0->kind == FAKE_GPIO_EV_SET_DIRECTION,
                   "the recorded event is SET_DIRECTION -- if this fails, relay_owner's "
                   "init sequence changed again and this test's header comment needs "
                   "re-checking against the new call order");
        TEST_CHECK(e0->dir == HAL_GPIO_DIR_OUT, "direction is switched to OUT");
    }
}

void run_test_relay_owner_gpio_init(void)
{
    test_relay_owner_start_energizes_nothing_and_latches_before_direction();
}
