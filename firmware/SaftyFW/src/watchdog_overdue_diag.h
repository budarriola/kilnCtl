// watchdog_overdue_diag.h -- reset-surviving latch for "which task(s)
// missed their check-in deadline, by how much", the CLEAR_TRIP-reboots-the-
// Pico investigation's actual conclusion: the drain block was never
// crashing (clear_trip_diag.h's checkpoint proved that, stage=4/outcome=2,
// a clean refusal that ran to completion), the reset is the hardware
// watchdog firing because something -- still unidentified -- made ONE
// iteration of a check-in-bound task take long enough to miss its own
// deadline in watchdog_gate.c's per-task gate.
//
// Repurposes watchdog_hw->scratch[5] (startup_diag.h's
// SAFTYFW_LAST_CHECKIN_MASK_SCRATCH): that register used to be rewritten
// UNCONDITIONALLY every watchdog_task_fn() evaluation (live state, not a
// latch), which is exactly the problem the coordinator identified -- by the
// time anyone reads it over SWD after a reset, the fresh boot's own healthy
// evaluations have already overwritten whatever the pre-reset value was.
// Nothing in this firmware ever reads scratch[5] back at runtime (grepped:
// the only two references before this change were the write in
// watchdog_task.c and the doc comment in startup_diag.h) -- it existed
// purely as SWD-readable forensic output, so repurposing its WRITE POLICY
// is safe: it is written ONLY when watchdog_task_fn() decides to withhold
// the feed (the one moment this diagnostic exists to capture), tagged with
// a magic byte the same way clear_trip_diag_codec.h tags scratch[7], so a
// stale value from a PRIOR reset cycle's already-read-and-cleared latch is
// never confused with a fresh one. Same "read then clear, early in main(),
// before anything (the scheduler, in this case) can touch it again" pattern
// as boot_reason.c and clear_trip_diag.h.
#ifndef SAFTYFW_WATCHDOG_OVERDUE_DIAG_H
#define SAFTYFW_WATCHDOG_OVERDUE_DIAG_H

#include "watchdog_overdue_diag_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Encodes and writes watchdog_hw->scratch[5] in one packed word. Called
// from watchdog_task_fn() ONLY on the branch where it decides NOT to feed
// the hardware watchdog -- never on the healthy branch, which is exactly
// the change from the old unconditional-every-evaluation write. Never
// blocks, never allocates (a plain MMIO write), safe to call moments before
// the hardware watchdog resets the chip.
void watchdog_overdue_diag_mark(uint8_t overdue_mask, uint8_t worst_task_id,
                                 uint16_t worst_overage_ms);

// Reads scratch[5], decodes it, and caches the result. Call once, early in
// main(), in the same boot step as boot_reason_read()/clear_trip_diag_read()
// -- BEFORE watchdog_overdue_diag_clear() (this is the only chance to see
// what the previous boot left behind) and BEFORE vTaskStartScheduler()
// brings watchdog_task_fn() up to write a fresh, healthy value over it.
watchdog_overdue_diag_t watchdog_overdue_diag_read(void);

// Returns whatever the one watchdog_overdue_diag_read() call this boot
// passed in -- same "cached once at boot" contract as
// clear_trip_diag_get_cached()/boot_reason_get_cached(). Named distinctly
// from clear_trip_diag's own cache (the coordinator's own note, after
// disassembling to find clear_trip_diag's: two identically-named s_cached
// statics are ambiguous in nm/objdump symbol listings).
watchdog_overdue_diag_t watchdog_overdue_diag_get_cached(void);

// Zeros scratch[5] so a future, unrelated watchdog-withheld-feed does not
// get read as this boot's stale value, and so the fresh boot's own healthy
// evaluations (which no longer write this register at all, per the new
// only-on-withhold policy) start from a clean, magic-untagged slate. Clears
// whichever format (overdue or overflow, see below) was actually present --
// there is only the one physical register, one clear covers both.
void watchdog_overdue_diag_clear(void);

// 2026-08-23, round 2: reads and decodes the SAME scratch[5] register as
// watchdog_overdue_diag_t's own format, but for the sibling event a stack
// overflow leaves there (see watchdog_overflow_diag_t's own doc comment,
// watchdog_overdue_diag_codec.h). Call alongside watchdog_overdue_diag_read()
// in main() -- at most one of the two will ever report magic_ok == true for
// a given boot, since the two events are mutually exclusive by construction.
// There is deliberately no watchdog_overflow_diag_mark(): the write side
// lives inline in main.c's vApplicationStackOverflowHook(), never through
// this wrapper, for the same "must not call into another compilation unit
// from a possibly-corrupted stack" reason the codec header documents.
watchdog_overflow_diag_t watchdog_overflow_diag_read(void);

// Returns whatever the one watchdog_overflow_diag_read() call this boot
// passed in -- same "cached once at boot" contract as every other _get_cached()
// in this codebase.
watchdog_overflow_diag_t watchdog_overflow_diag_get_cached(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_WATCHDOG_OVERDUE_DIAG_H
