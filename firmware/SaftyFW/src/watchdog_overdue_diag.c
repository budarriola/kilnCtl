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
    // 2026-09-10, opus review finding A: this firmware is
    // configNUMBER_OF_CORES 2 (FreeRTOSConfig.h) and watchdog_task runs
    // pinned to SAFTYFW_CORE_TRIP_PATH while every other task (link_task,
    // log_task, update_task) runs pinned to SAFTYFW_CORE_LINK_PATH
    // (watchdog_task.c / link_task.c / log_task.c / update_task.c). A fatal
    // hook (stack overflow / malloc-fail / configASSERT) on the OTHER core
    // disables interrupts and hangs THAT core only -- it does not stop
    // watchdog_task from continuing to run on its own core, noticing the
    // hung core's task missed its check-in, and reaching this function.
    // Before this guard, that call unconditionally overwrote whatever fatal
    // tag (0xE3/0xB4/0xA5) the hung core had just latched with this format's
    // own 0xD9 tag -- erasing the one piece of forensic evidence the fatal
    // hooks exist to leave, roughly 700ms before the unfed hardware watchdog
    // reset the chip, so the boot banner reported "check-in overdue" instead
    // of the real fault kind and line. The old single-core reasoning ("the
    // two events are mutually exclusive by construction") assumed only one
    // core exists; it does not hold with SMP.
    //
    // Fix: read back whatever is currently latched and refuse to overwrite
    // it if it already decodes as one of the three fatal formats. This read
    // is safe here (unlike in the fatal hooks themselves) -- watchdog_task
    // is an ordinary FreeRTOS task with an intact stack, calling into
    // hal_scratch through the normal function-call path.
    uint32_t existing_raw = 0u;
    // hal_scratch_read_u32()'s own return is intentionally not checked here,
    // same as the read in watchdog_fatal_diag_read() below and the write
    // below it: a failed read leaves existing_raw at its 0-initialised
    // value, which watchdog_fatal_diag_decode() reads as
    // WATCHDOG_FATAL_KIND_NONE (no magic byte matches 0), so the guard falls
    // through to writing the overdue latch -- the same outcome as "nothing
    // was there to protect". There is no more defensive action available to
    // a diagnostic register on a read failure here: this call runs on the
    // trip path itself, moments before a hardware watchdog reset, with no
    // error-reporting channel of its own.
    (void)hal_scratch_read_u32(WATCHDOG_OVERDUE_DIAG_SCRATCH, &existing_raw,
                                WATCHDOG_OVERDUE_DIAG_SCRATCH, 0u, NULL);
    watchdog_fatal_diag_t existing_fatal = watchdog_fatal_diag_decode(existing_raw);
    if (existing_fatal.kind != (uint8_t)WATCHDOG_FATAL_KIND_NONE) {
        // A fatal tag from the OTHER core is already latched this boot --
        // leave it alone. This boot's overdue-mask/worst-task/worst-overage
        // detail is real but strictly less useful than "which fatal fault
        // killed the board", so it is deliberately dropped rather than
        // fought over the one shared register.
        return;
    }

    // Same "no further recourse on this path" justification as the read
    // above -- a failed write here is a diagnostic-register write, not a
    // safety-relevant one; the watchdog reset that follows is unaffected
    // either way.
    (void)hal_scratch_write_u32(
        WATCHDOG_OVERDUE_DIAG_SCRATCH,
        watchdog_overdue_diag_encode(overdue_mask, worst_task_id, worst_overage_ms));
}

void watchdog_overdue_diag_notify_recovered(void)
{
    // 2026-09-10, opus review finding A (transient-overdue half): a mark()
    // above records a real event, but watchdog_task_fn()'s healthy branch
    // never called back in here, so a transient miss that recovered before
    // the hardware watchdog fired (a task briefly overran, then caught up
    // and fed on schedule again) left the 0xD9 overdue tag latched in
    // scratch[5] until the NEXT reset -- possibly hours later, for an
    // unrelated reason -- which would then misreport "check-in overdue" for
    // a boot in which nothing was actually overdue.
    //
    // Called from watchdog_task_fn()'s all_ok branch (the same branch that
    // feeds the hardware watchdog), this clears the latch ONLY if it is
    // still tagged as THIS format's own 0xD9 overdue mark -- never a fatal
    // tag. That check matters: unlike the mark() guard above, this path has
    // no read-first-then-decide-not-to-write ordering concern with a fatal
    // hook on the other core, because a fatal hook that has actually fired
    // has permanently hung its own core and that core's task(s) will never
    // check in again, so watchdog_task will keep taking the OVERDUE branch
    // (and calling mark(), which itself refuses to clobber the fatal tag)
    // rather than ever reaching this all_ok path again this boot. Checking
    // the tag anyway is just defence in depth, same discipline as the rest
    // of this module's "never trust a derived value over the raw facts"
    // comments.
    uint32_t existing_raw = 0u;
    (void)hal_scratch_read_u32(WATCHDOG_OVERDUE_DIAG_SCRATCH, &existing_raw,
                                WATCHDOG_OVERDUE_DIAG_SCRATCH, 0u, NULL);
    watchdog_overdue_diag_t existing = watchdog_overdue_diag_decode(existing_raw);
    if (existing.magic_ok) {
        (void)hal_scratch_clear(WATCHDOG_OVERDUE_DIAG_SCRATCH);
    }
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
