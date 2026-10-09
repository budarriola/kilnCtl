/* test_fake_time.c -- standalone MSVC host test for hwAbstraction/host/fake_time.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <stddef.h>

#include "fake_time.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void)
{
    fake_time_reset_all();

    /* --- starts at zero --- */
    CHECK(hal_time_now_us() == 0);
    CHECK(hal_time_now_ms() == 0);
    CHECK(fake_time_now_us() == 0);

    /* --- advance_us, non-round amount (avoid idealized-step inputs) --- */
    fake_time_advance_us(987);
    CHECK(hal_time_now_us() == 987);
    CHECK(fake_time_now_us() == 987);
    CHECK(hal_time_now_ms() == 0); /* still < 1ms */

    fake_time_advance_us(13);
    CHECK(hal_time_now_us() == 1000);
    CHECK(hal_time_now_ms() == 1);

    /* --- advance_ms --- */
    fake_time_reset_all();
    fake_time_advance_ms(7);
    CHECK(hal_time_now_us() == 7000);
    CHECK(hal_time_now_ms() == 7);

    /* --- dithered sequence of advances, not uniform steps --- */
    fake_time_reset_all();
    uint64_t expect = 0;
    uint64_t steps[] = {1, 999, 2, 501, 3001, 17};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        fake_time_advance_us(steps[i]);
        expect += steps[i];
        CHECK(hal_time_now_us() == expect);
    }

    /* --- delay_ms advances the clock (never a real sleep) --- */
    fake_time_reset_all();
    CHECK(hal_time_delay_ms(250) == HAL_OK);
    CHECK(hal_time_now_us() == 250000);
    CHECK(hal_time_now_ms() == 250);

    /* --- delay_ms(0) is a legal no-op advance --- */
    uint64_t before = hal_time_now_us();
    CHECK(hal_time_delay_ms(0) == HAL_OK);
    CHECK(hal_time_now_us() == before);

    /* --- multiple delays accumulate --- */
    fake_time_reset_all();
    hal_time_delay_ms(10);
    hal_time_delay_ms(20);
    hal_time_delay_ms(5);
    CHECK(hal_time_now_us() == 35000);
    CHECK(hal_time_now_ms() == 35);

    /* --- reset_all rewinds to zero --- */
    fake_time_advance_ms(1000);
    CHECK(hal_time_now_us() != 0);
    fake_time_reset_all();
    CHECK(hal_time_now_us() == 0);
    CHECK(hal_time_now_ms() == 0);

    /* --- large advance does not silently wrap a 64-bit counter --- */
    fake_time_reset_all();
    fake_time_advance_us(0xFFFFFFFFULL); /* > 32 bits of microseconds */
    CHECK(hal_time_now_us() == 0xFFFFFFFFULL);
    fake_time_advance_us(1);
    CHECK(hal_time_now_us() == 0x100000000ULL);

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
