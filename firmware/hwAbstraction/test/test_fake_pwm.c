/* test_fake_pwm.c -- standalone MSVC host test for
 * hwAbstraction/host/fake_pwm.c. Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fake_pwm.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void) {
    fake_pwm_reset_all();

    /* --- before init: everything refuses --- */
    CHECK(!fake_pwm_is_initialized());
    CHECK(hal_pwm_set_duty(50) == HAL_NOT_READY);
    CHECK(hal_pwm_deinit() == HAL_NOT_READY);
    {
        hal_pwm_cfg_t cfg;
        CHECK(!fake_pwm_get_init_cfg(&cfg));
    }
    CHECK(fake_pwm_get_duty_history_count() == 0);
    CHECK(fake_pwm_get_last_duty() == 0xFF);
    CHECK(fake_pwm_get_duty_history(0) == 0xFF);

    /* --- init: bad args, mirroring hal_pwm_esp.c's own validation --- */
    CHECK(hal_pwm_init(NULL) == HAL_INVALID_ARG);
    {
        hal_pwm_cfg_t bad = { 4, 5000, 0 }; /* duty_resolution_bits == 0 */
        CHECK(hal_pwm_init(&bad) == HAL_INVALID_ARG);
        bad.duty_resolution_bits = 21; /* > 20, the real LEDC_TIMER_BIT_MAX ceiling */
        CHECK(hal_pwm_init(&bad) == HAL_INVALID_ARG);
    }
    CHECK(!fake_pwm_is_initialized());

    /* --- init: good cfg is recorded verbatim --- */
    hal_pwm_cfg_t cfg;
    cfg.gpio_num = 4;
    cfg.freq_hz = 5000;
    cfg.duty_resolution_bits = 13;
    CHECK(hal_pwm_init(&cfg) == HAL_OK);
    CHECK(fake_pwm_is_initialized());
    {
        hal_pwm_cfg_t got;
        memset(&got, 0, sizeof(got));
        CHECK(fake_pwm_get_init_cfg(&got));
        CHECK(got.gpio_num == 4);
        CHECK(got.freq_hz == 5000);
        CHECK(got.duty_resolution_bits == 13);
    }

    /* --- set_duty: rejects out-of-range, matching the header's 0..100
     * contract --- */
    CHECK(hal_pwm_set_duty(101) == HAL_INVALID_ARG);
    CHECK(hal_pwm_set_duty(255) == HAL_INVALID_ARG);
    CHECK(fake_pwm_get_duty_history_count() == 0); /* rejected calls are not recorded */

    /* --- set_duty: accepted values recorded in order --- */
    CHECK(hal_pwm_set_duty(0) == HAL_OK);
    CHECK(hal_pwm_set_duty(50) == HAL_OK);
    CHECK(hal_pwm_set_duty(100) == HAL_OK);
    CHECK(fake_pwm_get_duty_history_count() == 3);
    CHECK(fake_pwm_get_duty_history(0) == 0);
    CHECK(fake_pwm_get_duty_history(1) == 50);
    CHECK(fake_pwm_get_duty_history(2) == 100);
    CHECK(fake_pwm_get_last_duty() == 100);
    CHECK(fake_pwm_get_duty_history(3) == 0xFF); /* out of range */

    /* --- history cap: does not overflow past FAKE_PWM_MAX_DUTY_HISTORY --- */
    {
        uint32_t i;
        for (i = 0; i < FAKE_PWM_MAX_DUTY_HISTORY + 10; i++) {
            CHECK(hal_pwm_set_duty((uint8_t)(i % 101)) == HAL_OK);
        }
        CHECK(fake_pwm_get_duty_history_count() == FAKE_PWM_MAX_DUTY_HISTORY);
    }

    /* --- deinit: succeeds once, refuses a second time, blocks set_duty --- */
    CHECK(hal_pwm_deinit() == HAL_OK);
    CHECK(!fake_pwm_is_initialized());
    CHECK(hal_pwm_deinit() == HAL_NOT_READY);
    CHECK(hal_pwm_set_duty(50) == HAL_NOT_READY);

    /* --- reset_all clears cfg and history --- */
    fake_pwm_reset_all();
    CHECK(!fake_pwm_is_initialized());
    CHECK(fake_pwm_get_duty_history_count() == 0);
    CHECK(fake_pwm_get_last_duty() == 0xFF);

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
