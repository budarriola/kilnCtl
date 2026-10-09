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

/* Minimum time the relay must stay energized once the time-proportioned
 * (PID) path has commanded it ON -- 10 s, set by the owner 2026-08-28.
 *
 * This is a FLOOR, not a default: heater_output_duty() raises any
 * cfg->min_on_ms below it up to it, so no per-zone configuration, backup
 * import or HTTP edit can schedule an on-pulse shorter than this. It is
 * enforced twice, because the window quantization alone is not enough:
 *   1. when a window's on-time is computed, an on-time below the floor
 *      renders as OFF for that window (the pre-existing min_on_ms rule);
 *   2. as a running hold -- once the relay is actually ON, an off decision
 *      is deferred until 10 s of continuous on-time has accumulated. This
 *      is what covers a window_ms shorter than the floor, and a duty that
 *      collapses between one window and the next.
 *
 * It deliberately does NOT apply to heater_output_force_off(), which is the
 * path every safety trip, halt, pause and stop uses. A trip must open the
 * contacts on the tick it happens; delaying a de-energize to protect relay
 * contacts would trade a safety property for a wear property. Nor does it
 * apply to heater_output_bangbang(), whose own min_on_ms/min_off_ms
 * debounce is the caller's to choose -- this is the PID relay's floor. */
#define HEATER_MIN_ON_MS_FLOOR 10000u

/* How many times the effective min-on-time the time-proportioning window has
 * to be before a real fractional duty can be rendered in it (2026-08-29).
 *
 * The floor above and window_ms below are not independent numbers, and
 * treating them as if they were is what broke this board's zone 0. With a
 * 2000 ms window and the 10 s floor, EVERY on-time a duty could compute --
 * 0.4*2000 = 800 ms, 0.9*2000 = 1800 ms -- is below the floor, so
 * heater_output_duty()'s quantization renders every one of them as OFF. The
 * zone is not a slow proportional controller at that point, it is a dead
 * one: autotune's step at duty 0.4 commanded heat for 40 minutes and the
 * relay never closed once. The reverse case is just as bad -- a window only
 * slightly longer than the floor can express nothing between "off" and
 * "almost always on".
 *
 * The rule enforced at config time is therefore
 *
 *     window_ms >= HEATER_MIN_WINDOW_MULTIPLE * max(min_on_ms, floor)
 *
 * which is the same thing as saying the smallest duty this zone can render
 * is 1/HEATER_MIN_WINDOW_MULTIPLE. At 3 that is 0.333: a zone can always
 * express roughly a third, two thirds and full duty, which is enough
 * resolution for a PID loop and for an autotune step to produce a real
 * thermal response. Below 3 the controller degenerates towards bang-bang
 * without anything telling the operator it has.
 *
 * 3 is a practical floor, not a control-theory optimum. Real windows are
 * much longer than the minimum it implies -- HEATER_DEFAULT_WINDOW_MS is
 * 60 s against a 10 s floor, a ratio of 6, giving duty steps of ~0.17. */
#define HEATER_MIN_WINDOW_MULTIPLE 3u

/* Firmware defaults used when a zone has no per-zone heater timing
 * configured (0 = not configured). Single home for the values that
 * profile_executor.c and autotune_engine.c both substitute. */
#define HEATER_DEFAULT_WINDOW_MS  60000u
#define HEATER_DEFAULT_MIN_ON_MS  HEATER_MIN_ON_MS_FLOOR
#define HEATER_DEFAULT_MIN_OFF_MS 2000u

typedef struct {
    /* time-proportioning window; default HEATER_DEFAULT_WINDOW_MS (60000).
     *
     * The old justification for 60 s -- "(mechanical relay)", meaning the
     * on-board relay's contact life -- is WRONG and was retired 2026-08-24.
     * K1-K4 are EE2-12NUH signal relays that exist for galvanic isolation and
     * switch other relays only; they never carry element current (ROADMAP.md's
     * decisions table, TODO.md 6A.0). Their own loaded life is 1e6 operations,
     * which a 10 s window would not exhaust for ~2,800 hours of CONTINUOUS
     * firing -- so this relay is not what constrains the window.
     *
     * 60 s is KEPT anyway, on a different and still-open reason: the
     * DOWNSTREAM device is unknown. Into an SSR input the window could drop
     * to a few seconds with no wear cost at all; into a mechanical contactor
     * (1e5-1e6 electrical ops) a 10 s window buys only ~280-2,800 hours, and
     * the low end of that is marginal. Picking a shorter default before that
     * is known would be trading real hardware life for control resolution
     * on an assumption, which is the trade this comment got wrong once
     * already. Revisit once TODO.md 6A.0's downstream question is answered;
     * it is a per-zone setting, so a bench can lower it without a rebuild. */
    uint32_t window_ms;
    /* Minimum on-time. In heater_output_duty() this is raised to at least
     * HEATER_MIN_ON_MS_FLOOR (10 s); a smaller configured value has no
     * effect there. Default HEATER_DEFAULT_MIN_ON_MS. */
    uint32_t min_on_ms;
    uint32_t min_off_ms;  /* default HEATER_DEFAULT_MIN_OFF_MS (2000) */
} heater_output_cfg_t;

typedef struct {
    bool     relay_on;
    bool     window_started;    /* time-proportioned mode: false until the first window's on-time is computed */
    uint32_t window_elapsed_ms; /* time-proportioned mode: position within the current window */
    uint32_t on_ms_this_window; /* time-proportioned mode: on-time computed for the current window */
    bool     last_commanded_on; /* bang-bang mode: for min-on/min-off debounce */
    uint32_t since_last_change_ms;
    uint32_t on_elapsed_ms; /* time-proportioned mode: continuous on-time, for the HEATER_MIN_ON_MS_FLOOR hold */
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
 * window instead. The effective min-on is max(cfg->min_on_ms,
 * HEATER_MIN_ON_MS_FLOOR), and once the relay is on an off decision is
 * additionally held until HEATER_MIN_ON_MS_FLOOR of continuous on-time has
 * accumulated. dt_ms is the elapsed time since the previous call.
 * Returns the relay state to actually command this tick. */
bool heater_output_duty(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                        uint32_t dt_ms);

/* Same rendering as heater_output_duty(), except when level_changed is true
 * the current window ends and a fresh one begins on THIS call, using this
 * call's duty, instead of waiting for window_ms to elapse.
 *
 * Why this exists (autotune_engine.c's relay-feedback identification,
 * 2026-09-01): the relay law flips its commanded duty the instant the
 * measurement crosses the hysteresis band, and that instant is what defines
 * Tu -- the whole point of the method. Routing that decision through
 * heater_output_duty()'s ordinary window logic means the ALREADY-COMPUTED
 * on_ms_this_window keeps being honoured until the window boundary, so a
 * flip that lands early in a window is not reflected in what the relay
 * actually does for up to window_ms (60 s by default) -- comparable to, or
 * longer than, this plant's identified dead time (34-53 s, PID_EXPANSION_PLAN
 * Sec 2). pid_autotune_fit_relay() measures Tu and the oscillation amplitude
 * from the recorded temperature trace, which is driven by the ACTUAL relay
 * state, not the logical decision -- so up to a window's worth of jitter,
 * uncorrelated between edges, lands directly in the period and amplitude it
 * fits, comfortably capable of exceeding RELAY_PERIOD_SPREAD_MAX (20%) and
 * RELAY_AMPLITUDE_SPREAD_MAX (35%) every run.
 *
 * Pass level_changed = true only on the tick the caller's own bang-bang law
 * actually changed which duty it wants (i.e. relay_law_tick()'s edge, in
 * either direction); every other tick this behaves exactly like
 * heater_output_duty(). The min_on_ms/min_off_ms quantization, floor and
 * running min-on hold are unchanged -- this only removes the window
 * boundary's own added delay on top of them. */
bool heater_output_duty_relay_step(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                                    uint32_t dt_ms, bool level_changed);

/* The shortest window_ms that can render a real fractional duty against the
 * given configured min_on_ms: HEATER_MIN_WINDOW_MULTIPLE * the EFFECTIVE
 * min-on, i.e. max(min_on_ms, HEATER_MIN_ON_MS_FLOOR). Config validation
 * (zones_http.c) and the on-load raise both compute the bound from here so
 * there is one definition of it. */
uint32_t heater_output_required_window_ms(uint32_t min_on_ms);

/* False when cfg->window_ms is non-zero but shorter than
 * heater_output_required_window_ms(cfg->min_on_ms) -- i.e. this zone cannot
 * express a real fractional duty and heater_output_duty() will only ever
 * render all-or-nothing on it. Defense in depth behind the config-layer
 * refusal: a caller that builds a heater_output_cfg_t from somewhere
 * zones_http.c does not police can check this and say so, rather than
 * silently running a bang-bang controller an operator asked to be a PID one.
 * A window_ms of 0 is "caller substitutes the default" and reports true. */
bool heater_output_cfg_expressible(const heater_output_cfg_t *cfg);

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
 * timing. Bypasses HEATER_MIN_ON_MS_FLOOR's hold by design -- a safety
 * de-energize is never delayed for contact wear. Does NOT reset cycle_count (that's a lifetime counter). Counts a
 * transition if the relay was on. */
void heater_output_force_off(heater_output_state_t *state);

/* The caller decided NOT to honour the ON this tick's heater_output_duty()/
 * heater_output_bangbang() call returned (profile_executor.c's
 * max_simultaneous_relays load cap, 2026-09-27). Those calls already
 * recorded relay_on = true and counted a transition for a relay that is now
 * not going to close, so without this the state would say "on" for a relay
 * that is off: the min-on hold would then start defending an on-time that
 * never happened, and cycle_count would drift from what the contacts did.
 * relay_on_before_tick/cycle_count_before_tick are the values from BEFORE
 * this tick's decision call (the caller snapshots them); the result is what
 * that call would have left had it decided OFF itself -- relay_on false,
 * on_elapsed_ms 0, and one transition counted only if the relay really was
 * on before (which also restarts bang-bang's since_last_change_ms, so the
 * min_off_ms hold covers the real OFF). Window timing
 * (window_elapsed_ms/on_ms_this_window) is deliberately left alone: the denied on-time is repaid through the
 * caller's deferred_on_ms credit, not by rewinding the window. */
void heater_output_note_denied(heater_output_state_t *state, bool relay_on_before_tick,
                               uint32_t cycle_count_before_tick);

#ifdef __cplusplus
}
#endif

#endif // HEATER_OUTPUT_H
