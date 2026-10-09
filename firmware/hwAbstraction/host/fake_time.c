/* fake_time.c -- host fake backend for hal_time.h. See fake_time.h. */
#include "fake_time.h"

static uint64_t s_clock_us = 0;

void fake_time_reset_all(void)
{
    s_clock_us = 0;
}

void fake_time_advance_us(uint64_t us)
{
    s_clock_us += us;
}

void fake_time_advance_ms(uint32_t ms)
{
    s_clock_us += (uint64_t)ms * 1000u;
}

uint64_t fake_time_now_us(void)
{
    return s_clock_us;
}

/* --- hal_time.h implementation --- */

uint64_t hal_time_now_us(void)
{
    return s_clock_us;
}

uint64_t hal_time_now_ms(void)
{
    return s_clock_us / 1000u;
}

hal_status_t hal_time_delay_ms(uint32_t ms)
{
    /* Never a real sleep on host -- advances the fake clock instead, per
     * hal_time.h's "vTaskDelay/sleep_ms replacement" and this fake's
     * "never real sleeps" contract. */
    s_clock_us += (uint64_t)ms * 1000u;
    return HAL_OK;
}
