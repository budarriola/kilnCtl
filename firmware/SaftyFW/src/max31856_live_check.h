// max31856_live_check.h -- pure cadence/episode-counting policy for
// periodically re-verifying that the safety MAX31856's CR0/CR1 registers
// still hold what this board last commissioned, while the part is currently
// reported verified. Split out the same way max31856_reconfig_retry.h is
// (no pico-sdk/FreeRTOS/SPI includes, directly host-tested with a plain
// poll counter -- test/test_max31856_live_check.c), for the same reason:
// thermo_task.c itself cannot be host-tested, so the decision logic has to
// live somewhere that can be.
//
// firmware/SaftyFW/docs/THERMOCOUPLE.md's completion checklist used to list
// "automatic config re-assertion if the part is ever seen to have reset" as
// not done: max31856_reconfig_retry.c only ever retries while
// max31856_tc_type_verified() is false, i.e. it has NEVER gone true yet (or
// a live SET_CONFIG tc_type change forced a fresh attempt). A part that
// verified once at boot and then resets itself mid-run -- reverting to its
// power-on CR0/CR1 defaults (Type K, AVGSEL off, CMODE off) -- keeps
// reporting max31856_tc_type_verified() == true forever after that, because
// nothing re-checks the live register contents against what was last
// written. This module is the missing periodic check's cadence: "every N
// polls, while verified, ask the caller to do one cheap CR0/CR1 readback and
// compare it against the shadow max31856.c already keeps
// (max31856_verify_live_config())."
//
// Deliberately narrow, same shape as max31856_reconfig_retry.h: this decides
// WHEN to check and counts mismatch episodes. It never calls
// max31856_configure() or touches SPI itself -- thermo_task_fn() does that,
// exactly like it already does for the reconfig-retry cadence.
#ifndef SAFTYFW_MAX31856_LIVE_CHECK_H
#define SAFTYFW_MAX31856_LIVE_CHECK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cadence: elapsed-time based, same pattern as max31856_reconfig_retry.h's
// next_attempt_due_ms (caller passes its own xTaskGetTickCount()-derived
// now_ms). Time-based, not poll-count-based, on purpose: thermo_task's loop
// period is NOT constant -- a configured part ticks at ~conv_ms via DRDY,
// but a part whose DRDY has gone silent (e.g. right after a reset clears
// CMODE) instead waits conv_ms * THERMO_TASK_DRDY_SILENCE_MULTIPLIER per
// iteration, not conv_ms. A poll-count cadence of 200 in exactly that
// silent/reset scenario this module exists to catch spans far closer to 60s
// of wall clock than 30s -- landing on S5's 60s blind_grace_s
// (safety_guards.h) trip boundary instead of safely beating it. Counting
// real elapsed ms instead fixes the interval at
// MAX31856_LIVE_CHECK_INTERVAL_MS regardless of which branch the loop is
// looping through, giving a real ~30s margin ahead of that 60s trip.
#define MAX31856_LIVE_CHECK_INTERVAL_MS 30000u

typedef struct {
    uint32_t next_check_due_ms; // caller's now_ms clock, valid only while `armed`
    bool     armed;             // true once `verified` has been observed true this episode
    uint32_t mismatch_count;    // cumulative NEW mismatch CHECKS (not just episodes -- see below)
    bool     mismatch_active;   // true while the current episode is unresolved
} max31856_live_check_state_t;

// Resets state to "about to start counting toward the first check" -- call
// once at boot, after max31856_reconfig_retry_init().
void max31856_live_check_init(max31856_live_check_state_t *state);

// Call once per thermo_task loop iteration, after this tick's
// max31856_tc_type_verified() result is known (post any reconfig-retry
// attempt already made this same iteration), passing the caller's current
// xTaskGetTickCount()-derived now_ms (same clock max31856_reconfig_retry
// uses). Returns true iff the caller should perform a fresh CR0/CR1 live
// readback (max31856_verify_live_config()) and report the result via
// max31856_live_check_note_result() below.
//
// While `verified` is false, the cadence clock is disarmed (not merely
// paused) so that the FIRST tick after the part becomes verified again
// (whether via the reconfig-retry path or a live SET_CONFIG reapply) arms a
// fresh full MAX31856_LIVE_CHECK_INTERVAL_MS interval starting from that
// moment, rather than firing immediately on stale progress toward a check
// that was never relevant to this configuration.
bool max31856_live_check_tick(max31856_live_check_state_t *state, bool verified, uint32_t now_ms);

// Call immediately after a check should_check told the caller to make.
// `match` is whether the live readback equalled the shadow
// (max31856_verify_live_config() returned MAX31856_LIVE_CHECK_MATCH); pass
// false only for a genuine MISMATCH, never for a READ_FAILED transfer (an
// SPI hiccup is not evidence the part reset -- the caller should simply skip
// reporting anything and let the next scheduled check retry).
//
// Always increments mismatch_count on a mismatch (a running total of
// mismatched CHECKS, not just episodes -- so a part that resets and is
// immediately reconfigured back to matching, then resets again ten minutes
// later, is visible as 2 in this counter, not 1). Returns true only the
// first time a NEW episode starts (mismatch_active was false), which is
// what the caller should gate its one-line WARN log on; the caller should
// call max31856_configure() again on EVERY mismatch this reports,
// regardless of the returned bool.
bool max31856_live_check_note_result(max31856_live_check_state_t *state, bool match);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_LIVE_CHECK_H
