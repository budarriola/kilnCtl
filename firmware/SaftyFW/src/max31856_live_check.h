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

// Cadence: poll-count based, not time-based -- thermo_task's loop period
// already varies (a configured part ticks at ~151ms via DRDY, an
// unconfigured/DRDY-silent one waits THERMO_TASK_UNCONFIGURED_WAIT_MS ==
// 500ms per iteration), and this check is only ever armed while `verified`
// is true, which means a real conversion cadence is already running. 200
// polls at the configured ~151ms/conversion rate is ~30s between live
// register re-checks -- one extra 3-byte SPI transfer (address + CR0 + CR1)
// every ~30s is negligible next to the six-register burst max31856_read()
// already does every single conversion, while still catching a live reset
// well inside the timescale a human would notice a stuck reading.
#define MAX31856_LIVE_CHECK_INTERVAL_POLLS 200u

typedef struct {
    uint32_t polls_since_check; // ticks since the last live readback attempt
    uint32_t mismatch_count;    // cumulative NEW mismatch episodes detected
    bool     mismatch_active;   // true while the current episode is unresolved
} max31856_live_check_state_t;

// Resets state to "about to start counting toward the first check" -- call
// once at boot, after max31856_reconfig_retry_init().
void max31856_live_check_init(max31856_live_check_state_t *state);

// Call once per thermo_task loop iteration, after this tick's
// max31856_tc_type_verified() result is known (post any reconfig-retry
// attempt already made this same iteration). Returns true iff the caller
// should perform a fresh CR0/CR1 live readback (max31856_verify_live_config())
// and report the result via max31856_live_check_note_result() below.
//
// While `verified` is false, the cadence counter is held at 0 (not merely
// paused) so that the FIRST poll after the part becomes verified again
// (whether via the reconfig-retry path or a live SET_CONFIG reapply) starts
// a fresh full interval, rather than firing immediately on old, stale
// progress toward a check that was never relevant to this configuration.
bool max31856_live_check_tick(max31856_live_check_state_t *state, bool verified);

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
