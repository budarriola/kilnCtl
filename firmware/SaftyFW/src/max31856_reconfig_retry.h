// max31856_reconfig_retry.h -- pure retry-cadence policy for re-attempting
// max31856_configure() while the part has never (or no longer) verified its
// commissioned tc_type. Split out the same way max31856_tc_range_policy.h/
// max31856_tc_type_policy.h/max31856_decode.h are: no pico-sdk/FreeRTOS/
// SPI includes, so it is directly host-tested (test/test_max31856_reconfig_
// retry.c) with a fake clock, no hardware.
//
// TODO.md's bring-up note ("The safety MAX31856 must be present before the
// Pico boots"): main.c's step-6 max31856_configure() call runs exactly
// once, before vTaskStartScheduler(). If the IC is not yet powered/settled
// at that instant, that single attempt fails, max31856_tc_type_verified()
// stays false forever, and thermo_task.c's existing "!verified -> invalid"
// downgrade (see its own wiring comment) latches the board at "safety TC
// invalid" -> S5 trip 0x05 for the rest of the boot, recoverable only by a
// Pico reset. This file is the decision logic for retrying that one-shot
// probe periodically instead, from thermo_task's own tick, while the
// sensor is still flagged invalid -- so a boot-time IC power-up race
// becomes self-healing instead of a hard latch.
//
// Deliberately narrow: this decides WHEN to call max31856_configure() again
// and tracks how many times it has been tried. It never touches S5's
// trip-latch semantics (safety_guards.h/safety_core.c are untouched by this
// change) -- while verification is false the guard still sees tc_valid ==
// false and trips exactly as before; once a retry succeeds, the very next
// real reading is simply eligible to be valid again, the same way a fresh
// boot with the IC present already was. A trip that has already latched
// stays latched; this only changes whether the SENSOR can become usable
// again, not whether a latched trip auto-clears.
#ifndef SAFTYFW_MAX31856_RECONFIG_RETRY_H
#define SAFTYFW_MAX31856_RECONFIG_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cadence: bounded, per the bug report's own "every 500 ms-1 s from the
// existing tick" instruction. 500 ms chosen as the floor of that range --
// thermo_task's own DRDY-silence loop already ticks at roughly this rate
// while unconfigured (max31856_conversion_time_ms() falls back to
// THERMO_TASK_UNCONFIGURED_WAIT_MS == 500 ms in thermo_task.c), so this
// timer piggybacks on an interval the task already waits out rather than
// adding a busier one.
#define MAX31856_RECONFIG_RETRY_INTERVAL_MS 500u

// Bounded retry count, per the bug report's "then give up loudly" -- 20
// attempts at the 500 ms cadence above is 10 s of retrying, long enough to
// ride out a slow IC power-up/decoupling-cap settle, short enough that a
// truly absent/dead part does not retry forever silently disguising a real
// hardware fault as "still probing".
#define MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS 20u

typedef struct {
    uint32_t retry_count;         // attempts made so far this "invalid" episode
    uint32_t next_attempt_due_ms; // xTaskGetTickCount()-derived ms tick, caller's clock
    bool     gave_up;             // true once retry_count reached the bound with no success
} max31856_reconfig_retry_state_t;

// Resets state to "about to start retrying" -- call once at boot, after the
// initial max31856_init()/max31856_configure() probe in main.c (whether
// that probe succeeded or not; if it succeeded, verified will read true on
// the first should_attempt() call below and no retry ever fires).
void max31856_reconfig_retry_init(max31856_reconfig_retry_state_t *state);

// Call once per thermo_task loop iteration, BEFORE deciding whether to
// treat this cycle's reading as valid. `verified` is the current
// max31856_tc_type_verified() result; `now_ms` is the caller's own
// monotonic ms tick (xTaskGetTickCount() * portTICK_PERIOD_MS on-target, a
// fake counter in the host test).
//
// Returns true iff the caller should call max31856_configure(tc_type) right
// now and then report the fresh result via max31856_reconfig_retry_note_
// result(). Returns false when: already verified (nothing to retry),
// already gave up (bound reached), or the retry interval has not elapsed
// yet since the last attempt.
bool max31856_reconfig_retry_should_attempt(max31856_reconfig_retry_state_t *state, bool verified,
                                             uint32_t now_ms);

// Call immediately after an attempt should_attempt() told the caller to
// make, with the fresh max31856_tc_type_verified() result post-configure().
// On success, clears retry_count/gave_up so a LATER loss of verification
// (should that ever become possible) gets its own fresh retry budget
// rather than inheriting a stale gave_up flag from a previous episode. On
// failure, advances the retry count and the next-attempt deadline, setting
// gave_up once the bound is reached.
void max31856_reconfig_retry_note_result(max31856_reconfig_retry_state_t *state, bool verified,
                                          uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_RECONFIG_RETRY_H
