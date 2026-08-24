// clear_trip_diag.c -- see clear_trip_diag.h. Thin wrapper: encode/decode
// (clear_trip_diag_codec.c, pure, host-tested) on one side,
// watchdog_hw->scratch[7] on the other, nothing else.
#include "clear_trip_diag.h"

#include "hardware/structs/watchdog.h"

#define CLEAR_TRIP_DIAG_SCRATCH 7u // the one free register -- see clear_trip_diag.h's own budget comment

// Written only by clear_trip_diag_read(), before the scheduler starts --
// same single-writer-at-boot contract as boot_reason.c's s_cached -- named distinctly (per the coordinator's own note, after disassembling to find this one) so the two are never ambiguous in nm/objdump.
static clear_trip_diag_t s_clear_trip_diag_cached;

void clear_trip_diag_mark(uint8_t stage, uint8_t reason, uint8_t fault_bits, bool tc_valid,
                           bool spi_failed, bool tc_c_is_nan, uint8_t outcome)
{
    watchdog_hw->scratch[CLEAR_TRIP_DIAG_SCRATCH] =
        clear_trip_diag_encode(stage, reason, fault_bits, tc_valid, spi_failed, tc_c_is_nan, outcome);
}

clear_trip_diag_t clear_trip_diag_read(void)
{
    clear_trip_diag_t out = clear_trip_diag_decode(watchdog_hw->scratch[CLEAR_TRIP_DIAG_SCRATCH]);
    s_clear_trip_diag_cached = out;
    return out;
}

clear_trip_diag_t clear_trip_diag_get_cached(void)
{
    return s_clear_trip_diag_cached;
}

void clear_trip_diag_clear(void)
{
    watchdog_hw->scratch[CLEAR_TRIP_DIAG_SCRATCH] = 0;
}
