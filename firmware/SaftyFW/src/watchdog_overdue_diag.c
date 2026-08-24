// watchdog_overdue_diag.c -- see watchdog_overdue_diag.h. Thin wrapper:
// encode/decode (watchdog_overdue_diag_codec.c, pure, host-tested) on one
// side, watchdog_hw->scratch[5] on the other, nothing else.
#include "watchdog_overdue_diag.h"

#include "hardware/structs/watchdog.h"

#define WATCHDOG_OVERDUE_DIAG_SCRATCH 5u // startup_diag.h's SAFTYFW_LAST_CHECKIN_MASK_SCRATCH, repurposed -- see this file's own header comment

// Written only by watchdog_overdue_diag_read(), before the scheduler
// starts -- same single-writer-at-boot contract as boot_reason.c's
// s_cached / clear_trip_diag.c's own (distinctly named, per the
// coordinator's note).
static watchdog_overdue_diag_t s_watchdog_overdue_cached;
// 2026-08-23, round 2 -- see watchdog_overflow_diag_read()'s own header
// comment. Distinctly named from s_watchdog_overdue_cached above, same
// nm/objdump-disambiguation discipline as clear_trip_diag.c's own rename.
static watchdog_overflow_diag_t s_watchdog_overflow_cached;

void watchdog_overdue_diag_mark(uint8_t overdue_mask, uint8_t worst_task_id,
                                 uint16_t worst_overage_ms)
{
    watchdog_hw->scratch[WATCHDOG_OVERDUE_DIAG_SCRATCH] =
        watchdog_overdue_diag_encode(overdue_mask, worst_task_id, worst_overage_ms);
}

watchdog_overdue_diag_t watchdog_overdue_diag_read(void)
{
    watchdog_overdue_diag_t out =
        watchdog_overdue_diag_decode(watchdog_hw->scratch[WATCHDOG_OVERDUE_DIAG_SCRATCH]);
    s_watchdog_overdue_cached = out;
    return out;
}

watchdog_overdue_diag_t watchdog_overdue_diag_get_cached(void)
{
    return s_watchdog_overdue_cached;
}

void watchdog_overdue_diag_clear(void)
{
    watchdog_hw->scratch[WATCHDOG_OVERDUE_DIAG_SCRATCH] = 0;
}

watchdog_overflow_diag_t watchdog_overflow_diag_read(void)
{
    watchdog_overflow_diag_t out =
        watchdog_overflow_diag_decode(watchdog_hw->scratch[WATCHDOG_OVERDUE_DIAG_SCRATCH]);
    s_watchdog_overflow_cached = out;
    return out;
}

watchdog_overflow_diag_t watchdog_overflow_diag_get_cached(void)
{
    return s_watchdog_overflow_cached;
}
