// clear_trip_diag.c -- see clear_trip_diag.h. Thin wrapper: encode/decode
// (clear_trip_diag_codec.c, pure, host-tested) on one side,
// watchdog_hw->scratch[7] on the other, nothing else.
#include "clear_trip_diag.h"

#include <stddef.h> // NULL, for the hal_scratch_read_u32() magic_ok arg below

// HAL Phase 3 item 1: routed through hal_scratch.h instead of poking
// watchdog_hw->scratch[] directly -- see docs/HW_ABSTRACTION_PLAN.md
// "hal_scratch -- pico watchdog-scratch registry". This module owns slot 7
// (claimed in main.c alongside the other real owners). The packed word's
// own magic tag (clear_trip_diag_codec.c) is unrelated to hal_scratch's
// read-time magic-word parameter -- no separate scratch slot pairs with
// this one, so slot 7 is passed for both `slot` and `magic_slot` below and
// *magic_ok is ignored, per hal_scratch.h's own convention for that case.
#include "hal_scratch.h"

#define CLEAR_TRIP_DIAG_SCRATCH 7u // the one free register -- see clear_trip_diag.h's own budget comment

// Written only by clear_trip_diag_read(), before the scheduler starts --
// same single-writer-at-boot contract as boot_reason.c's s_cached -- named distinctly (per the coordinator's own note, after disassembling to find this one) so the two are never ambiguous in nm/objdump.
static clear_trip_diag_t s_clear_trip_diag_cached;

void clear_trip_diag_mark(uint8_t stage, uint8_t reason, uint8_t fault_bits, bool tc_valid,
                           bool spi_failed, bool tc_c_is_nan, uint8_t outcome)
{
    (void)hal_scratch_write_u32(
        CLEAR_TRIP_DIAG_SCRATCH,
        clear_trip_diag_encode(stage, reason, fault_bits, tc_valid, spi_failed, tc_c_is_nan, outcome));
}

clear_trip_diag_t clear_trip_diag_read(void)
{
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(CLEAR_TRIP_DIAG_SCRATCH, &raw,
                                CLEAR_TRIP_DIAG_SCRATCH, 0u, NULL);
    clear_trip_diag_t out = clear_trip_diag_decode(raw);
    s_clear_trip_diag_cached = out;
    return out;
}

clear_trip_diag_t clear_trip_diag_get_cached(void)
{
    return s_clear_trip_diag_cached;
}

void clear_trip_diag_clear(void)
{
    (void)hal_scratch_clear(CLEAR_TRIP_DIAG_SCRATCH);
}
