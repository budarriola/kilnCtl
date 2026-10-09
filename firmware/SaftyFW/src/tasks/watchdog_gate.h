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

// Gate AND feed, in one place: evaluates exactly the same deadline rule
// watchdog_gate_all_within_deadline() implements, and calls `feed` if and
// only if that rule says every entry is within its own deadline. Returns the
// same decision and reports the same ok_mask, so a caller can still drive its
// own side effects (the heartbeat LED, the overdue diagnostic latch) off the
// one result rather than recomputing the decision independently.
//
// WHY THIS EXISTS (2026-09-18,
// docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md): the hardware feed
// used to have exactly one caller, watchdog_task_fn(), which is a FreeRTOS
// task pinned to SAFTYFW_CORE_TRIP_PATH and therefore cannot be scheduled at
// all while hal_flash_safe_execute() holds BOTH cores with interrupts
// disabled. update_task_erase_slot() has to be able to issue the feed itself,
// between sector erases, or the chip resets partway through a slot erase.
// What must NOT change when the feed gains a second owner is the POLICY: a
// feed is still allowed only when every registered task is within its own
// deadline, so a genuinely wedged safety processor still starves the watchdog
// and still reboots. Keeping the gate and the feed in ONE function is what
// holds that true for both callers -- CLAUDE.md's "reset one side of a pair"
// class is exactly what two hand-copied gate-then-feed sequences would become
// the next time the rule changes on one side only.
//
// `feed` is called at most once per invocation, and never at all when any
// entry is past its own deadline. A NULL `feed` evaluates the gate and skips
// the call.
bool watchdog_gate_feed_if_all_within_deadline(const watchdog_gate_entry_t *entries, uint32_t count,
                                                uint32_t *out_ok_mask, void (*feed)(void));

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_WATCHDOG_GATE_H
