// Settable hardware-timer clock for the main host exe (current_sense.c). The
// FreeRTOS tick stub in this exe is a separate, fixed clock, so a test can prove
// a snapshot is stamped from THIS clock and not from the tick (A2, 2026-10-14).
#include "pico/time.h"
#include "test_pico_time_clock.h"

static uint32_t s_hw_ms = 1u;

void test_hw_clock_set_ms(uint32_t ms) { s_hw_ms = ms; }

absolute_time_t get_absolute_time(void)
{
    absolute_time_t t;
    t.us = (uint64_t)s_hw_ms * 1000u;
    return t;
}

uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t.us / 1000u); }
