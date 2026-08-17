// boot_reason.c -- see boot_reason.h. Two watchdog scratch registers:
// scratch[0] = trip reason, scratch[1] = magic word. Validated before trusted
// (docs/ARCHITECTURE.md section 8: "the registers are uninitialised after a
// true power-on").
#include "boot_reason.h"

#include "hardware/structs/watchdog.h"

#define SAFTYFW_TRIP_REASON_SCRATCH   0
#define SAFTYFW_TRIP_MAGIC_SCRATCH    1
// Arbitrary, just needs to be astronomically unlikely to appear in
// power-on-uninitialised SRAM by chance.
#define SAFTYFW_TRIP_MAGIC_WORD       0x53414654u // 'SAFT'

saftyfw_boot_reason_t boot_reason_read(bool wd_caused_reboot, bool wd_enable_caused_reboot)
{
    saftyfw_boot_reason_t out = {
        .watchdog_caused_reboot = wd_caused_reboot,
        .watchdog_enable_caused_reboot = wd_enable_caused_reboot,
        .trip_reason_valid = false,
        .trip_reason = 0,
    };

    if (watchdog_hw->scratch[SAFTYFW_TRIP_MAGIC_SCRATCH] == SAFTYFW_TRIP_MAGIC_WORD) {
        out.trip_reason_valid = true;
        out.trip_reason = watchdog_hw->scratch[SAFTYFW_TRIP_REASON_SCRATCH];
    }

    return out;
}

void boot_reason_latch_trip(uint32_t trip_reason)
{
    watchdog_hw->scratch[SAFTYFW_TRIP_REASON_SCRATCH] = trip_reason;
    watchdog_hw->scratch[SAFTYFW_TRIP_MAGIC_SCRATCH] = SAFTYFW_TRIP_MAGIC_WORD;
}

void boot_reason_clear_trip(void)
{
    watchdog_hw->scratch[SAFTYFW_TRIP_MAGIC_SCRATCH] = 0;
}
