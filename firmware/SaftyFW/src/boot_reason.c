// boot_reason.c -- see boot_reason.h. Two watchdog scratch registers:
// scratch[0] = trip reason, scratch[1] = magic word. Validated before trusted
// (docs/ARCHITECTURE.md section 8: "the registers are uninitialised after a
// true power-on").
#include "boot_reason.h"

// HAL Phase 3 item 1: routed through hal_scratch.h instead of poking
// watchdog_hw->scratch[] directly -- see docs/HW_ABSTRACTION.md
// "hal_scratch -- pico watchdog-scratch registry". This module owns
// slots 0/1 (claimed in main.c alongside the other real owners).
#include "hal_scratch.h"

#define SAFTYFW_TRIP_REASON_SCRATCH   0
#define SAFTYFW_TRIP_MAGIC_SCRATCH    1
// Arbitrary, just needs to be astronomically unlikely to appear in
// power-on-uninitialised SRAM by chance.
#define SAFTYFW_TRIP_MAGIC_WORD       0x53414654u // 'SAFT'

// Written once, by boot_reason_read(), before the scheduler starts (main.c
// step 3) -- see boot_reason.h's doc comment on boot_reason_get_cached().
static saftyfw_boot_reason_t s_cached;

saftyfw_boot_reason_t boot_reason_read(bool wd_caused_reboot, bool wd_enable_caused_reboot)
{
    saftyfw_boot_reason_t out = {
        .watchdog_caused_reboot = wd_caused_reboot,
        .watchdog_enable_caused_reboot = wd_enable_caused_reboot,
        .trip_reason_valid = false,
        .trip_reason = 0,
    };

    uint32_t trip_reason_raw = 0u;
    bool magic_ok = false;
    (void)hal_scratch_read_u32(SAFTYFW_TRIP_REASON_SCRATCH, &trip_reason_raw,
                                SAFTYFW_TRIP_MAGIC_SCRATCH, SAFTYFW_TRIP_MAGIC_WORD,
                                &magic_ok);
    if (magic_ok) {
        out.trip_reason_valid = true;
        out.trip_reason = trip_reason_raw;
    }

    s_cached = out;
    return out;
}

saftyfw_boot_reason_t boot_reason_get_cached(void)
{
    return s_cached;
}

void boot_reason_latch_trip(uint32_t trip_reason)
{
    (void)hal_scratch_write_u32(SAFTYFW_TRIP_REASON_SCRATCH, trip_reason);
    (void)hal_scratch_write_u32(SAFTYFW_TRIP_MAGIC_SCRATCH, SAFTYFW_TRIP_MAGIC_WORD);
}

void boot_reason_clear_trip(void)
{
    (void)hal_scratch_clear(SAFTYFW_TRIP_MAGIC_SCRATCH);
}
