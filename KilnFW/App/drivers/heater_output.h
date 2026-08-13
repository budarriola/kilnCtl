// heater_output -- turns a control decision (a PID/bang-bang duty, or a
// direct on/off) into relay commands. TODO.md section 6A.1.
//
// Pure state-in/decision-out like pid.h/thermal_guard.h: this module never
// touches kiln_io itself -- it returns "energize: yes/no" and the caller
// (profile_executor.c) is the one that calls kiln_io_set_relay_mask()
// through relay_authority. That split is what makes the min-on/min-off and
// cycle-counting logic testable without hardware.
//
// Two modes, matching the two control modes that produce a decision here:
//   - Bang-bang: caller passes want_on directly (from comparing actual vs
//     setpoint +/- hysteresis) -- heater_output_bangbang() just applies
//     min-on/min-off debounce and counts a transition.
//   - Time-proportioned: caller passes a duty in [0,1] from pid_update();
//     heater_output_duty() renders it onto a window_ms on/off cycle,
//     quantized by min_on_ms/min_off_ms per TODO.md 6A.1's explicit rule
//     that an unachievably short on-time renders as OFF for that window,
//     not as a rounded-up minimum pulse.
#ifndef HEATER_OUTPUT_H
#define HEATER_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t window_ms;  /* time-proportioning window; default 60000 (mechanical relay) */
    uint32_t min_on_ms;   /* default 2000 */
    uint32_t min_off_ms;  /* default 2000 */
} heater_output_cfg_t;

typedef struct {
    bool     relay_on;
    bool     window_started;    /* time-proportioned mode: false until the first window's on-time is computed */
    uint32_t window_elapsed_ms; /* time-proportioned mode: position within the current window */
    uint32_t on_ms_this_window; /* time-proportioned mode: on-time computed for the current window */
    bool     last_commanded_on; /* bang-bang mode: for min-on/min-off debounce */
    uint32_t since_last_change_ms;
    uint32_t cycle_count; /* relay ON->OFF or OFF->ON transitions, for contact-life accounting (6A.1) */
} heater_output_state_t;

void heater_output_reset(heater_output_state_t *state);

/* Bang-bang: want_on is the raw comparator decision (actual < setpoint -
 * hysteresis, etc). Debounces against min_on_ms/min_off_ms -- a want_on
 * flip inside either window is held at the previous state rather than
 * acted on immediately, which is what keeps a noisy reading right at the
 * hysteresis edge from chattering the relay. dt_ms is the elapsed time
 * since the previous call. Returns the relay state to actually command. */
bool heater_output_bangbang(heater_output_state_t *state, const heater_output_cfg_t *cfg, bool want_on,
                            uint32_t dt_ms);

/* Time-proportioned: duty in [0,1] from pid_update(). Renders onto
 * window_ms: on for duty*window_ms at the start of each window, off for the
 * rest, EXCEPT a computed on-time below min_on_ms renders as off for the
 * whole window (TODO.md 6A.1 -- not rounded up to min_on_ms, since that is
 * how a relay ends up chattering at low demand) and a computed off-time
 * below min_off_ms (i.e. duty very close to 1) renders as on for the whole
 * window instead. dt_ms is the elapsed time since the previous call.
 * Returns the relay state to actually command this tick. */
bool heater_output_duty(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                        uint32_t dt_ms);

/* TODO.md 6A.5's load-staggering item: call once right after
 * heater_output_reset(), before the first heater_output_duty() call, to
 * offset this instance's window boundary so N zones sharing the same
 * window_ms don't all start (and stop) their on-time simultaneously. The
 * first window is truncated to (window_ms - phase_offset_ms % window_ms)
 * and forced OFF to establish the offset; every window after that has the
 * normal window_ms period, so the phase shift is exact and permanent for
 * the life of this state (not just the first window). A no-op if
 * window_ms == 0 or phase_offset_ms % window_ms == 0. Meaningless for
 * heater_output_bangbang() (no window concept there) -- caller only needs
 * this for a zone in PID/time-proportioned mode. */
void heater_output_seed_phase(heater_output_state_t *state, uint32_t window_ms, uint32_t phase_offset_ms);

/* Forces the relay off right now (a safety trip, a pause, a stop) and
 * resets window/debounce timing -- the next heater_output_* call after this
 * starts a clean window rather than resuming mid-window against stale
 * timing. Does NOT reset cycle_count (that's a lifetime counter). Counts a
 * transition if the relay was on. */
void heater_output_force_off(heater_output_state_t *state);

#ifdef __cplusplus
}
#endif

#endif // HEATER_OUTPUT_H
