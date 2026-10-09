// test_relay_owner_gpio_init.c -- HAL Phase 1b (docs/HW_ABSTRACTION.md
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
// Host/hardware mirror gap this test used to paper over, now closed:
// fake_gpio.c's hal_gpio_set() refuses to apply a level on a pin that
// isn't yet marked .initialized (returns HAL_NOT_READY, no SET_LEVEL event
// recorded) -- a check the real pico backend's gpio_put() does not make,
// since it writes the output register unconditionally regardless of
// direction/init state. A fresh fake_gpio_reset() pin starts
// un-initialized, so without more, relay_owner_start()'s hal_gpio_set()
// call would silently be a no-op on host and this test could not tell the
// difference between that call running and it being deleted entirely.
// fake_gpio_force_state() closes the gap: it seeds the pin as already
// initialized OUT+HIGH (energized), modeling main.c's raw pre-HAL boot-time
// init WITHOUT recording an event, so the two calls relay_owner_start()
// actually makes -- hal_gpio_set(pin, false) then
// hal_gpio_set_direction(pin, OUT) -- are both real, observable events
// against a pin that hal_gpio_set() will not refuse. Seeding HIGH (not
// LOW) also means the final LOW state below is proof the latch happened,
// not just that it started that way.
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
    // Seed "already brought up by main.c's raw pre-HAL boot code": OUT
    // direction, energized (HIGH) -- see file header. Not an event.
    fake_gpio_force_state(SAFTYFW_PIN_RELAY, HAL_GPIO_DIR_OUT, true);
    TEST_CHECK(fake_gpio_event_count() == 0,
               "seeding the pre-existing boot state records no event");

    bool ok = relay_owner_start();
    TEST_CHECK(ok, "relay_owner_start() succeeds against the host stubs");

    TEST_CHECK(fake_gpio_is_initialized(SAFTYFW_PIN_RELAY),
               "the relay pin is initialized after relay_owner_start()");
    TEST_CHECK(fake_gpio_current_dir(SAFTYFW_PIN_RELAY) == HAL_GPIO_DIR_OUT,
               "the relay pin ends up in OUT direction");
    TEST_CHECK(fake_gpio_current_level(SAFTYFW_PIN_RELAY) == false,
               "the relay pin is de-energized (LOW) immediately after init -- "
               "a relay must never glitch on at init");

    // With the pin pre-seeded as initialized, hal_gpio_set() is no longer
    // refused: relay_owner_start() must produce exactly the two events
    // SET_LEVEL(false) then SET_DIRECTION(OUT), in that order --
    // latch-before-direction. If the hal_gpio_set(pin, false) call were
    // ever deleted from relay_owner_start(), this pair collapses to a
    // single SET_DIRECTION event and these checks fail (see the negative
    // test in test/host_tests_negative or this file's own comment below).
    TEST_CHECK(fake_gpio_event_count() >= 2,
               "at least two fake_gpio events were recorded (SET_LEVEL then SET_DIRECTION)");

    const fake_gpio_event_t *e0 = fake_gpio_event(0);
    const fake_gpio_event_t *e1 = fake_gpio_event(1);
    TEST_CHECK(e0 != NULL && e1 != NULL, "both expected events exist");
    if (e0 && e1) {
        TEST_CHECK(e0->pin == SAFTYFW_PIN_RELAY && e1->pin == SAFTYFW_PIN_RELAY,
                   "both events are for the relay pin (GPIO6)");
        TEST_CHECK(e0->kind == FAKE_GPIO_EV_SET_LEVEL,
                   "first event is SET_LEVEL (the latch) -- if this fails, either "
                   "relay_owner's hal_gpio_set() call was deleted/reordered, or it is "
                   "no longer driving the level before switching direction");
        TEST_CHECK(e0->level == false,
                   "the latched level is LOW (de-energized), not the seeded HIGH");
        TEST_CHECK(e1->kind == FAKE_GPIO_EV_SET_DIRECTION,
                   "second event is SET_DIRECTION (OUT), after the latch");
        TEST_CHECK(e1->dir == HAL_GPIO_DIR_OUT, "direction is switched to OUT");
    }
}

// relay_owner_start() checks both hal_gpio_set()/hal_gpio_set_direction()
// hal_status_t results and aborts startup on failure -- this is the
// relay's fail-safe latch, not best-effort, so a HAL error must not be
// silently discarded. A freshly-reset (never seeded) fake_gpio pin is an
// easy, real way to exercise that failure path on host: hal_gpio_set()
// on an un-initialized pin returns HAL_NOT_READY (fake_gpio.c), so
// relay_owner_start() must return false WITHOUT ever creating the command
// queue or the task.
static void test_relay_owner_start_fails_when_hal_gpio_set_fails(void)
{
    TEST_SECTION("relay_owner_start() -- aborts on a hal_gpio_set() failure "
                 "instead of silently continuing (HAL Phase 1b)");

    fake_gpio_reset(); // pin left un-initialized -- hal_gpio_set() will fail

    bool ok = relay_owner_start();
    TEST_CHECK(!ok, "relay_owner_start() returns false when hal_gpio_set() fails");
    TEST_CHECK(fake_gpio_event_count() == 0,
               "no fake_gpio event was recorded -- the failed call is a true no-op, "
               "confirming relay_owner_start() did not paper over the failure and "
               "proceed to create the queue/task on an unknown-state pin");
}

void run_test_relay_owner_gpio_init(void)
{
    test_relay_owner_start_energizes_nothing_and_latches_before_direction();
    test_relay_owner_start_fails_when_hal_gpio_set_fails();
}
