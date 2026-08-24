// watchdog_gate.h -- pure per-task check-in deadline evaluation.
//
// This is deliberately split out of watchdog_task.c and kept free of any
// FreeRTOS.h/pico-sdk include so it is directly host-testable
// (test/test_watchdog_gate.c links this .c file with no stub layer needed),
// unlike watchdog_task.c itself which needs real hardware/watchdog.h and
// hardware/gpio.h and can only be regression-tested by the source-text-scan
// trick test_watchdog_budget_coverage.c uses.
//
// The gate this replaces (docs/ARCHITECTURE.md section 4 / watchdog_task.c's
// original comment) required every registered task to check in inside the
// SAME SAFTYFW_PERIOD_WATCHDOG_TASK_MS (250 ms) window before a feed was
// allowed. That is structurally impossible to satisfy: thermo_task's own
// loop period (task_priorities.h has no fixed period for it; its real cadence
// is derived from max31856_conversion_time_ms(), and is 500 ms whenever the
// part is unconfigured -- true on this bench board today, the safety
// thermocouple IC is not fitted) is itself longer than 250 ms, so a perfectly
// healthy thermo_task cannot land in every window by construction. The board
// reset on the hardware watchdog every few seconds as a direct result.
//
// The fix: each task gets its OWN deadline, sized off its OWN real period
// (see watchdog_task.c's s_checkin_deadline_ms table for the numbers and the
// margin/cap reasoning), and the gate feeds iff every task is within its own
// deadline right now -- not "all landed in the same window".
#ifndef SAFTYFW_TASKS_WATCHDOG_GATE_H
#define SAFTYFW_TASKS_WATCHDOG_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One task's liveness fact at evaluation time: how long it has been since its
// last check-in, and the maximum silence it is allowed before it counts as
// stalled.
typedef struct {
    uint32_t elapsed_ms;
    uint32_t deadline_ms;
} watchdog_gate_entry_t;

// Evaluates `count` entries (count must be <= 32, one bit per entry below).
// Returns true (feed) iff EVERY entry's elapsed_ms is <= its own deadline_ms
// -- strictly greater than the deadline is what "stalled" means, so equal is
// still fine (a task landing exactly on its deadline has not yet been silent
// longer than its own allowance).
//
// If out_ok_mask is non-NULL, *out_ok_mask gets bit i set for every entry
// that was within its own deadline, bit i clear for every entry that was
// not -- watchdog_task.c XORs this against WATCHDOG_CHECKIN_ALL_MASK to get
// the overdue set it latches via watchdog_overdue_diag_mark() (2026-08-23,
// scratch[5], repurposed -- see that module's own header comment) whenever
// it decides to withhold the feed, so after a starvation reboot the tasks
// that were over their own deadline at the last evaluation before the
// reset are still readable.
bool watchdog_gate_all_within_deadline(const watchdog_gate_entry_t *entries, uint32_t count,
                                        uint32_t *out_ok_mask);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_WATCHDOG_GATE_H
