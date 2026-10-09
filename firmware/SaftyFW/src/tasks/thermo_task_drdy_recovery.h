// thermo_task_drdy_recovery.h -- pure decision for recovering a missed
// ~DRDY falling edge, split out the same way max31856_tc_range_policy.h/
// max31856_reconfig_retry.h are: no pico-sdk/FreeRTOS includes, so it is
// directly host-tested (test/test_thermo_task_drdy_recovery.c) with no
// hardware or scheduler.
//
// docs/audits/safety_tc_drdy_stall_2026-09-08.md: the MAX31856's CMODE
// auto-conversion starts the instant main.c's pre-scheduler
// max31856_configure() returns -- well before thermo_task_fn() gets a
// chance to arm the ~DRDY GPIO IRQ (deliberately deferred until the
// scheduler is running, see thermo_task.c's own header comment on that
// ordering). If the first conversion completes and asserts ~DRDY (active
// low) before the IRQ is armed, that falling edge already happened with
// nobody watching -- an edge-triggered IRQ armed on an already-low pin
// never fires again, because the MAX31856 only releases ~DRDY on a host
// read of the data registers, and nothing will ever perform that read if
// it is gated behind the very notification that edge would have produced.
// This is a self-latching stall, observed for real on hardware (the audit
// above): ~DRDY sampled LOW, unchanging, across three independent reads.
//
// The fix is a level check, not a one-shot arm-time patch: DRDY is
// level-observable (thermo_task_fn() already polls on a timeout even
// without this fix), so the same check applied every time the notify-wait
// times out recovers from ANY missed edge, not just the boot-time race --
// closing the self-latch class generally rather than papering over the one
// instance caught on the bench.
#ifndef SAFTYFW_TASKS_THERMO_TASK_DRDY_RECOVERY_H
#define SAFTYFW_TASKS_THERMO_TASK_DRDY_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns true only when a completed-but-unread conversion is the honest
// explanation for a notify-wait timeout:
//   - notifications == 0            -- the wait genuinely produced nothing
//     (a real edge always takes thermo_task_fn()'s notified branch on its
//     own; this function is not even consulted otherwise).
//   - !assume_ready                 -- the bench-only SAFTYFW_THERMO_ASSUME_DRDY
//     path has not already claimed this iteration.
//   - drdy_pin_level == 0            -- ~DRDY itself reads asserted right now
//     (active low, matching thermo_drdy_isr()'s GPIO_IRQ_EDGE_FALL).
//
// The third condition is what keeps this from ever fabricating a reading
// for a genuinely silent/dead part: a part that never asserts ~DRDY at all
// leaves the pin HIGH on the board's external pull-up (R2), this returns
// false, and the caller falls straight through, unchanged, into the
// pre-existing DRDY-silence branch that publishes sensor-invalid and lets
// S5 see a real bad read -- exactly as it did before this fix existed.
//
// A part whose ~DRDY reads LOW but is actually faulty is not "recovered"
// into a good reading either: this function only decides whether to
// ATTEMPT the read at all. max31856_read()'s own ok/spi_failed/fault_status
// result and thermo_task.c's per-type plausibility check downstream are
// exactly as authoritative over the outcome as they are for any normal,
// DRDY-triggered read -- nothing about how the read got triggered changes
// what is done with its result.
bool thermo_task_drdy_missed_edge(uint32_t notifications, bool assume_ready,
                                   uint32_t drdy_pin_level);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_THERMO_TASK_DRDY_RECOVERY_H
