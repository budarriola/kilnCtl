#include "heater_output.h"

#include <math.h>
#include <string.h>

void heater_output_reset(heater_output_state_t *state)
{
    uint32_t cycles = state->cycle_count; /* lifetime counter survives a reset */
    memset(state, 0, sizeof(*state));
    state->cycle_count = cycles;
}

static void note_transition(heater_output_state_t *state, bool new_on)
{
    if (new_on != state->relay_on) {
        state->cycle_count++;
    }
    state->relay_on = new_on;
}

static bool heater_output_duty_ex(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                                   uint32_t dt_ms, bool force_new_window);

bool heater_output_bangbang(heater_output_state_t *state, const heater_output_cfg_t *cfg, bool want_on,
                            uint32_t dt_ms)
{
    state->since_last_change_ms += dt_ms;

    /* Gate on want_on vs the *relay's actual state*, not vs the previous
     * call's want_on. Gating on the latter (an earlier version of this
     * function) only re-checked the debounce timer on the exact call where
     * want_on changed -- if that call was too early, want_on then held
     * steady at the new value on every later call, which looked like "no
     * flip" and silently dropped the still-pending transition forever, even
     * though since_last_change_ms kept accumulating past min_ms. Comparing
     * against relay_on instead means a standing want_on!=relay_on gets
     * applied the moment enough time has actually elapsed, regardless of
     * whether the comparator kept flickering in between. */
    if (want_on != state->relay_on) {
        uint32_t min_ms = state->relay_on ? cfg->min_on_ms : cfg->min_off_ms;
        if (state->since_last_change_ms >= min_ms) {
            note_transition(state, want_on);
            state->since_last_change_ms = 0;
        }
        /* else: too soon -- hold at the current relay_on and wait for the
         * next call; since_last_change_ms keeps accumulating either way. */
    }
    state->last_commanded_on = want_on; /* telemetry only now; no longer used for gating */
    return state->relay_on;
}

bool heater_output_duty(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                        uint32_t dt_ms)
{
    return heater_output_duty_ex(state, cfg, duty, dt_ms, false);
}

bool heater_output_duty_relay_step(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                                    uint32_t dt_ms, bool level_changed)
{
    return heater_output_duty_ex(state, cfg, duty, dt_ms, level_changed);
}

static bool heater_output_duty_ex(heater_output_state_t *state, const heater_output_cfg_t *cfg, float duty,
                                   uint32_t dt_ms, bool force_new_window)
{
    /* NaN fails both ordered comparisons, so test the positive form: a NaN duty (a PID
     * fed a bad reading) is OFF, never passed on to the on-time quantizer (firing review
     * 2026-10-09 item 5). */
    if (!isfinite(duty) || !(duty > 0.0f)) {
        duty = 0.0f;
    } else if (duty > 1.0f) {
        duty = 1.0f;
    }

    /* The floor the owner set (2026-08-28): a configured min_on_ms below
     * HEATER_MIN_ON_MS_FLOOR is raised to it here rather than at the config
     * layer, so every caller and every stored config gets it. */
    uint32_t min_on_ms = cfg->min_on_ms;
    if (min_on_ms < HEATER_MIN_ON_MS_FLOOR) {
        min_on_ms = HEATER_MIN_ON_MS_FLOOR;
    }
    /* A window shorter than the floor is a configuration the two constraints
     * cannot both satisfy inside one window. Since 2026-08-29 the config
     * layer refuses to store one (window_ms >= HEATER_MIN_WINDOW_MULTIPLE *
     * effective min_on -- see heater_output_cfg_expressible() and
     * ZONE_HEATER_WINDOW_MIN_MULTIPLE in zones_http.h), so this branch is
     * unreachable from any policed path and exists only so a cfg built
     * outside that policing still behaves predictably rather than
     * unpredictably: the quantization uses at most the window length, so a
     * high duty renders as a full-window ON and the running hold below keeps
     * it ON across as many consecutive windows as it takes to reach 10 s.
     * What it CANNOT do is render a low duty -- that is the degradation the
     * config-layer rule now prevents rather than papers over. */
    if (min_on_ms > cfg->window_ms) {
        min_on_ms = cfg->window_ms;
    }

    state->window_elapsed_ms += dt_ms;
    if (!state->window_started || state->window_elapsed_ms >= cfg->window_ms || force_new_window) {
        /* New window (or the very first call): compute this window's
         * on-time from the current duty and quantize per TODO.md 6A.1's
         * explicit rule. */
        state->window_started = true;
        state->window_elapsed_ms = 0;
        uint32_t on_ms = (uint32_t)(duty * (float)cfg->window_ms);
        if (on_ms < min_on_ms) {
            on_ms = 0; /* unachievably short -- render as off, not rounded up */
        } else if (cfg->window_ms - on_ms < cfg->min_off_ms) {
            on_ms = cfg->window_ms; /* symmetric case: duty near 1 */
        }
        state->on_ms_this_window = on_ms;
    }

    bool want_on = state->window_elapsed_ms < state->on_ms_this_window;

    /* Running min-on hold. The window quantization above cannot cover every
     * case on its own: a window_ms shorter than the floor, or a duty that
     * drops to 0 in the window after an on-pulse started, would still let a
     * sub-floor pulse reach the contacts. Once the relay is actually on, an
     * off decision waits until HEATER_MIN_ON_MS_FLOOR of continuous on-time
     * has accumulated -- across window boundaries if need be.
     *
     * Only routine PID cycling passes through here. A trip, halt, pause or
     * stop calls heater_output_force_off(), which does not consult this. */
    if (state->relay_on) {
        state->on_elapsed_ms += dt_ms;
        if (!want_on && state->on_elapsed_ms < HEATER_MIN_ON_MS_FLOOR) {
            want_on = true;
        }
    } else if (want_on) {
        state->on_elapsed_ms = 0;
    }

    note_transition(state, want_on);
    return state->relay_on;
}

void heater_output_seed_phase(heater_output_state_t *state, uint32_t window_ms, uint32_t phase_offset_ms)
{
    if (window_ms == 0) return;
    phase_offset_ms %= window_ms;
    if (phase_offset_ms == 0) return;
    state->window_started = true;
    state->window_elapsed_ms = phase_offset_ms;
    state->on_ms_this_window = 0;
}

void heater_output_force_off(heater_output_state_t *state)
{
    note_transition(state, false);
    state->window_started = false;
    state->window_elapsed_ms = 0;
    state->on_ms_this_window = 0;
    state->last_commanded_on = false;
    state->since_last_change_ms = 0;
    state->on_elapsed_ms = 0;
}

void heater_output_note_denied(heater_output_state_t *state, bool relay_on_before_tick,
                               uint32_t cycle_count_before_tick)
{
    /* Rebuild the "decided OFF" outcome from the pre-tick snapshot rather
     * than decrementing: the decision call may or may not have counted a
     * transition (it did iff relay_on flipped), and the only thing that
     * decides whether the physical relay switches this tick is where it
     * was before. */
    state->cycle_count = cycle_count_before_tick + (relay_on_before_tick ? 1u : 0u);
    state->relay_on = false;
    state->on_elapsed_ms = 0;
    /* Review fix (reset-one-side class): bang-bang's min_off_ms debounce
     * derives from since_last_change_ms. A denial of a relay that really was
     * ON is a real ON->OFF switch, so the off-time starts now; without this
     * a re-grant next tick could close the contacts again inside min_off_ms.
     * (A denial from OFF leaves whatever the decision call left: bangbang()
     * zeroed it on its phantom flip, which only delays the next ON.) */
    if (relay_on_before_tick) {
        state->since_last_change_ms = 0;
    }
}

uint32_t heater_output_required_window_ms(uint32_t min_on_ms)
{
    uint32_t effective = (min_on_ms < HEATER_MIN_ON_MS_FLOOR) ? HEATER_MIN_ON_MS_FLOOR : min_on_ms;
    return effective * HEATER_MIN_WINDOW_MULTIPLE;
}

bool heater_output_cfg_expressible(const heater_output_cfg_t *cfg)
{
    if (!cfg || cfg->window_ms == 0) {
        return true; /* 0 = caller substitutes HEATER_DEFAULT_WINDOW_MS */
    }
    return cfg->window_ms >= heater_output_required_window_ms(cfg->min_on_ms);
}
