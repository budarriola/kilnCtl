/* test_fake_wdt.c -- standalone MSVC host test for
 * hwAbstraction/host/fake_wdt.c. Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "fake_wdt.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void) {
    fake_wdt_reset_all();

    /* --- before init: feed/set_panic_disabled refuse, nothing fires --- */
    CHECK(!fake_wdt_is_initialized());
    CHECK(hal_wdt_feed() == HAL_NOT_READY);
    CHECK(hal_wdt_set_panic_disabled(true) == HAL_NOT_READY);
    fake_wdt_advance_ms(1000000u); /* un-armed: must not fire */
    CHECK(!fake_wdt_fired());
    CHECK(fake_wdt_get_feed_count() == 0);

    /* --- init records cfg --- */
    CHECK(hal_wdt_init(500, false) == HAL_OK);
    CHECK(fake_wdt_is_initialized());
    CHECK(fake_wdt_get_timeout_ms() == 500);
    CHECK(fake_wdt_get_panic_disabled() == false);
    CHECK(fake_wdt_get_feed_count() == 0);
    CHECK(!fake_wdt_fired());

    /* --- feed count increments, resets elapsed --- */
    CHECK(hal_wdt_feed() == HAL_OK);
    CHECK(fake_wdt_get_feed_count() == 1);
    CHECK(hal_wdt_feed() == HAL_OK);
    CHECK(fake_wdt_get_feed_count() == 2);

    /* --- advancing under the timeout does not fire --- */
    fake_wdt_advance_ms(400);
    CHECK(!fake_wdt_fired());
    fake_wdt_advance_ms(100); /* now at exactly 500, not > 500 -- must not fire */
    CHECK(!fake_wdt_fired());
    fake_wdt_advance_ms(1); /* now 501 > 500 -- fires */
    CHECK(fake_wdt_fired());

    /* --- fired is sticky until a feed or re-init --- */
    fake_wdt_advance_ms(1);
    CHECK(fake_wdt_fired());
    CHECK(hal_wdt_feed() == HAL_OK); /* real firmware can still feed after a near-miss */
    CHECK(!fake_wdt_fired());        /* feed clears the latch */
    CHECK(fake_wdt_get_feed_count() == 3);

    /* --- re-init also clears the latch and resets feed count --- */
    fake_wdt_advance_ms(1000);
    CHECK(fake_wdt_fired());
    CHECK(hal_wdt_init(1000, true) == HAL_OK);
    CHECK(!fake_wdt_fired());
    CHECK(fake_wdt_get_feed_count() == 0);
    CHECK(fake_wdt_get_timeout_ms() == 1000);
    CHECK(fake_wdt_get_panic_disabled() == true);

    /* --- set_panic_disabled updates the recorded flag once armed --- */
    CHECK(hal_wdt_set_panic_disabled(false) == HAL_OK);
    CHECK(fake_wdt_get_panic_disabled() == false);

    /* --- reboot latches, does not crash the test process --- */
    CHECK(!fake_wdt_reboot_requested());
    hal_wdt_reboot();
    CHECK(fake_wdt_reboot_requested());

    /* --- reset_all clears everything, including reboot latch --- */
    fake_wdt_reset_all();
    CHECK(!fake_wdt_is_initialized());
    CHECK(!fake_wdt_fired());
    CHECK(!fake_wdt_reboot_requested());
    CHECK(fake_wdt_get_feed_count() == 0);
    CHECK(fake_wdt_get_timeout_ms() == 0);
    CHECK(fake_wdt_get_panic_disabled() == false);

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
