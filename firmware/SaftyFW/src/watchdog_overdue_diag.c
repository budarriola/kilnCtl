// watchdog_overdue_diag.c -- see watchdog_overdue_diag.h. Thin wrapper:
// encode/decode (watchdog_overdue_diag_codec.c, pure, host-tested) on one
// side, watchdog_hw->scratch[5] on the other, nothing else.
#include "watchdog_overdue_diag.h"

#include <stddef.h> // NULL, for the hal_scratch_read_u32() magic_ok arg below

// HAL Phase 3 item 1: routed through hal_scratch.h instead of poking
// watchdog_hw->scratch[] directly -- see docs/HW_ABSTRACTION.md
// "hal_scratch -- pico watchdog-scratch registry". This module claims
// slot 5 under tag 0xD9; main.c's vApplicationStackOverflowHook is slot 5's
// other legitimate co-owner (tag 0xE3, claimed in main.c) but that write
// path deliberately stays a raw watchdog_hw->scratch[] MMIO write -- it can
// run moments after a stack overflow, where even hal_scratch_write_u32()'s
// ordinary function-call overhead is not safe to assume (see main.c's own
// comment on that hook for why). Reads here (both formats share the one
// physical register) are safe to route through hal_scratch: they only ever
// run at boot, on the one thread of execution, well before the scheduler.
#include "hal_scratch.h"

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
// 2026-09-09, the fatal-fault latch (stack overflow / malloc failure /
// configASSERT). Distinctly named for the same nm/objdump reason.
static watchdog_fatal_diag_t s_watchdog_fatal_cached;

void watchdog_overdue_diag_mark(uint8_t overdue_mask, uint8_t worst_task_id,
                                 uint16_t worst_overage_ms)
{
    (void)hal_scratch_write_u32(
        WATCHDOG_OVERDUE_DIAG_SCRATCH,
        watchdog_overdue_diag_encode(overdue_mask, worst_task_id, worst_overage_ms));
}

watchdog_overdue_diag_t watchdog_overdue_diag_read(void)
{
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(WATCHDOG_OVERDUE_DIAG_SCRATCH, &raw,
                                WATCHDOG_OVERDUE_DIAG_SCRATCH, 0u, NULL);
    watchdog_overdue_diag_t out = watchdog_overdue_diag_decode(raw);
    s_watchdog_overdue_cached = out;
    return out;
}

watchdog_overdue_diag_t watchdog_overdue_diag_get_cached(void)
{
    return s_watchdog_overdue_cached;
}

void watchdog_overdue_diag_clear(void)
{
    (void)hal_scratch_clear(WATCHDOG_OVERDUE_DIAG_SCRATCH);
}

watchdog_overflow_diag_t watchdog_overflow_diag_read(void)
{
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(WATCHDOG_OVERDUE_DIAG_SCRATCH, &raw,
                                WATCHDOG_OVERDUE_DIAG_SCRATCH, 0u, NULL);
    watchdog_overflow_diag_t out = watchdog_overflow_diag_decode(raw);
    s_watchdog_overflow_cached = out;
    return out;
}

watchdog_overflow_diag_t watchdog_overflow_diag_get_cached(void)
{
    return s_watchdog_overflow_cached;
}

// 2026-09-09: the unified fatal-fault read. Same physical register, same
// read-once-at-boot contract, same cache discipline as the two above --
// see watchdog_fatal_diag_t's doc comment (watchdog_overdue_diag_codec.h)
// for why one register serves all three fatal formats. This supersedes
// watchdog_overflow_diag_read() for callers that want "what killed the last
// boot" rather than specifically "did a stack overflow kill it"; the
// overflow-specific pair stays for its existing callers and because it is
// the one format carrying task-name bytes.
watchdog_fatal_diag_t watchdog_fatal_diag_read(void)
{
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(WATCHDOG_OVERDUE_DIAG_SCRATCH, &raw,
                                WATCHDOG_OVERDUE_DIAG_SCRATCH, 0u, NULL);
    watchdog_fatal_diag_t out = watchdog_fatal_diag_decode(raw);
    s_watchdog_fatal_cached = out;
    return out;
}

watchdog_fatal_diag_t watchdog_fatal_diag_get_cached(void)
{
    return s_watchdog_fatal_cached;
}
