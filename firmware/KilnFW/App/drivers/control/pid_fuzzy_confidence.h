// pid_fuzzy_confidence -- the confidence gate specified in
// docs/ADAPTIVE_FUZZY_EVALUATION.md sec 3. Pure C, no FreeRTOS, no
// ESP-IDF, no logging, no I/O, no globals -- same host-testable discipline
// as pid_fuzzy.c/pid.c.
//
// The central point (plan sec 1.4, restated here so it is not lost by
// whoever next edits this file): the seven known limit-cycling cells are
// A6 = MATCHED, i.e. the plant MODEL IS CORRECT there. A confidence gate
// keyed on model agreement would read HIGH confidence exactly where the
// harm occurs. This is why the authority cap below is keyed on L/tau (the
// ratio of identified dead time to identified time constant), never on
// how well the model fits -- model-fit quality is the OTHER signal (the
// consecutive-good-run counter, c), and the two are multiplied together,
// not substituted for each other. Do not "simplify" this into a single
// model-quality score; that reintroduces exactly the failure the plan's
// review found.
#ifndef PID_FUZZY_CONFIDENCE_H
#define PID_FUZZY_CONFIDENCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// c clamps to this range (plan sec 3.2: "clamped to [0,4]").
#define PID_FUZZY_CONFIDENCE_MAX_C 4u

// S_MAX in plan sec 3.2's formula -- today's shipped (and factorial-
// condemned-at-fixed-authority) default strength, now reachable only as a
// ceiling that must additionally be earned via c and cap_L.
#define PID_FUZZY_CONFIDENCE_S_MAX_PCT 50u

// (N2) The dead-time authority cap. Plan sec 3.2:
//   cap_L = 1.0                          for L/tau <= 0.10
//   cap_L = linear 1.0 -> 0.0            for 0.10 < L/tau <= 0.30
//   cap_L = 0.0                          for L/tau  > 0.30
//
// The 0.10/0.30 breakpoints are CHOSEN by desk reasoning, not measured --
// the plan says so explicitly and names the factorial run as the intended
// falsification instrument for them. Do not present them as measured.
//
// dead_time_s and tau_s both come from this zone's own autotune-identified
// FOPDT model (zone_model_at() at the call site) -- never from a fixture
// constant (plan sec 2). Fails SAFE (returns 0.0f, i.e. no authority) on
// any non-finite input, tau_s <= 0, or dead_time_s < 0: an unknown or
// invalid dead-time/time-constant ratio must never be read as "no cap
// needed."
float pid_fuzzy_confidence_cap_l(float dead_time_s, float tau_s);

// Plan sec 3.2: strength = round(S_MAX * (c/4) * cap_L).
// consecutive_good_runs is clamped to [0, PID_FUZZY_CONFIDENCE_MAX_C]
// internally -- callers do not have to pre-clamp. cap_l is clamped to
// [0.0, 1.0] internally for the same reason (a caller-side rounding error
// in cap_l must not push strength over S_MAX or negative).
//
// consecutive_good_runs == 0 (nothing earned yet) returns exactly 0
// regardless of cap_l, and cap_l == 0.0f (dead-time cap fully closed)
// returns exactly 0 regardless of consecutive_good_runs -- "low confidence
// on EITHER axis means plain PID" is a deliberate AND, not an average: a
// zone with a perfect fit-stability record but L/tau > 0.30 must not run
// fuzzy just because c is high (that is precisely the seven-cell failure
// mode), and a zone with a wonderful L/tau but zero earned confidence has
// nothing yet to run fuzzy on.
uint8_t pid_fuzzy_confidence_strength_pct(uint8_t consecutive_good_runs, float cap_l);

// (N3) In-firing error-zero-crossing oscillation backstop.
//
// Plan sec 3.2: "count error zero-crossings over a rolling window during a
// dwell. The measured contrast is stark -- 29 crossings in the oscillating
// arm, 0 in both stable arms -- so the threshold has enormous margin."
// PID_FUZZY_OSCILLATION_TRIP_CROSSINGS below is chosen well inside that
// margin (documented at its definition in the .c file), not measured on
// this codebase's own hardware -- this project's own standing lesson is to
// say so rather than imply a threshold this precise was fit to data.
//
// Implementation choice, disclosed rather than silent: this uses a
// TUMBLING window (crossings counted, then the counter reset every
// PID_FUZZY_OSCILLATION_WINDOW_S), not a true sliding window -- simpler,
// host-testable with no ring buffer, and adequate given the enormous
// measured margin (29 vs 0): a real limit cycle keeps producing crossings
// every window, so it cannot hide by straddling one window boundary the
// way a borderline count near the threshold could.
typedef struct {
    float    prev_error_c;
    bool     have_prev_error;
    uint32_t crossings_in_window;
    float    window_elapsed_s;
    // Sticky for the rest of the firing once set -- plan sec 3.2: "On trip:
    // strength_pct -> 0 for the remainder of the firing." Cleared only by
    // pid_fuzzy_oscillation_reset() at the next firing's start.
    bool     tripped_this_firing;
} pid_fuzzy_oscillation_state_t;

// Call once per firing, before that firing's first tick (profile_executor_
// run.c's per-zone run-start block, alongside pid_reset()/fuzzy_prev_
// effective_ki = 0.0f -- see that call site). Zeroes everything, including
// tripped_this_firing: a trip from the PREVIOUS firing must not silently
// carry into a new one (that would be the "reset one side of a pair" class
// this project has hit before, just inverted -- here the hazard is failing
// to reset a state a fresh firing needs to start clean).
void pid_fuzzy_oscillation_reset(pid_fuzzy_oscillation_state_t *st);

// Call once per control tick (pid_family_zone_tick()'s zone_commanded_
// setpoint_c(z, zi) - z->actual_c is the correct error_c to pass -- same
// signal pid_fuzzy_prepare_gains() already computes, see that call site).
// dt_s <= 0 or non-finite, or a non-finite error_c, is treated as a no-op
// tick (no crossing counted, no window time accrued, prev_error_c left
// untouched) -- a bad tick must not be misread as a real crossing, and
// must not silently reset the window early either.
//
// Returns true exactly on the tick that transitions tripped_this_firing
// from false to true (an edge, not a level) so a caller that wants to log
// or act once on the trip can do so; st->tripped_this_firing is the level
// value to consult on every other tick.
bool pid_fuzzy_oscillation_tick(pid_fuzzy_oscillation_state_t *st, float error_c, float dt_s);

#ifdef __cplusplus
}
#endif

#endif // PID_FUZZY_CONFIDENCE_H
