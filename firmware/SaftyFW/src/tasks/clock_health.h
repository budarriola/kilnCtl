// clock_health.h -- pure detector for a stalled get_absolute_time()/
// time_us_64() clock, split out of safety_core.c the same way
// watchdog_gate.h is split out of watchdog_task.c (see that header's own
// comment): free of pico-sdk/FreeRTOS includes so it is directly
// host-testable (test/test_clock_health.c links this .c file with no stub
// layer needed).
//
// 2026-08-23, the DIAG-content-frozen investigation: the RP2040's TIMER
// peripheral (which to_ms_since_boot(get_absolute_time()) reads) pauses
// whenever either core is halted by a debugger, by silicon default
// (TIMER_DBGPAUSE resets to 0x7) -- confirmed on the bench, uptime_ms frozen
// at the exact millisecond core 1 came up under an attached probe.
// main.c now clears that register at boot specifically so this stops
// happening (see its own comment for the full mechanism and the trade-off
// being made). This file is the belt to that fix's suspenders: it does not
// assume the register write took, survives a reflash, or holds on some
// future board/SDK/probe combination -- it detects a stalled clock directly
// from its own behaviour and lets the caller fail closed, so a safety-
// relevant staleness check (safety_core.c's context_valid and
// reboot_grace_active) cannot silently pass forever just because a register
// default went uncorrected somewhere. "A guard that silently passes because
// its clock stopped is the worst shape of bug in this codebase" -- the
// coordinator's own framing for why this exists as a permanent, independent
// check rather than trusting the main.c fix alone.
#ifndef SAFTYFW_TASKS_CLOCK_HEALTH_H
#define SAFTYFW_TASKS_CLOCK_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How many consecutive observations must show the clock failing to advance
// (by at least half of what a healthy tick should have added) before it is
// declared stalled. 1 would fire on a single anomalous read; this debounces
// against that while still catching a genuinely frozen clock within a
// handful of ticks -- at safety_core_task's own SAFTYFW_PERIOD_SAFETY_CORE_MS
// cadence this is at most a few hundred ms of detection latency, negligible
// against CONTEXT_MAX_AGE_MS/REBOOT_GRACE_WINDOW_MS (both measured in
// seconds).
#define CLOCK_HEALTH_STALL_DEBOUNCE 3u

// Caller-owned state, one instance per clock being watched (safety_core.c
// keeps exactly one, for its own now_ms reads). Zero-initialise before first
// use -- clock_health_observe()'s own doc comment covers what that means for
// the first call.
typedef struct {
    uint32_t last_now_ms;
    bool     have_last;
    uint32_t consecutive_stalled;
} clock_health_state_t;

// Call once per tick with the clock's current reading (e.g.
// to_ms_since_boot(get_absolute_time())) and dt_s -- the ACTUAL elapsed time
// since the previous call, from a source already known to be reliable
// (safety_core.c passes its own compile-time-constant dt_s, paced by
// vTaskDelayUntil() on the FreeRTOS tick, per that file's own comment on
// why dt_s does not itself depend on this clock).
//
// Returns true iff the clock should be treated as STALLED right now: it has
// failed to advance by at least half of dt_s's worth of milliseconds for
// CLOCK_HEALTH_STALL_DEBOUNCE consecutive calls in a row. A single call
// after state is reset (have_last == false) can never itself declare a
// stall -- there is nothing yet to compare against -- so this always
// returns false on the very first observation, exactly like every other
// "not enough history yet" default in this codebase.
//
// dt_s <= 0 is treated as "no expectation of advancement this call" and
// never contributes to a stall verdict (also cannot happen in practice --
// safety_core.c's dt_s is a fixed positive constant -- but a defensive
// caller should not have to prove that here).
//
// Wraparound-safe: now_ms is compared via unsigned subtraction, the same
// idiom safety_core.c's own reboot_grace_active/context_valid computations
// already use for the identical reason (both sides come from the same
// clock, so the difference wraps correctly in uint32_t arithmetic
// regardless of which side is numerically larger) -- this matters because a
// clock that eventually resumes from a stall must not be misread as having
// jumped backward.
bool clock_health_observe(clock_health_state_t *state, uint32_t now_ms, float dt_s);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_CLOCK_HEALTH_H
