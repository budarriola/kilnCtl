#include "heater_output.h"

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
    if (duty < 0.0f) {
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
     * cannot both satisfy inside one window. Rather than silently refusing to
     * heat at all -- which is what an un-clamped floor would do to a window
     * of, say, 4 s, since no on-time in it could ever reach 10 s -- the
     * quantization uses at most the window length, so a high duty still
     * renders as a full-window ON. The running hold below then keeps that ON
     * across as many consecutive windows as it takes to reach 10 s, so the
     * floor is honoured even though the window alone cannot express it. */
    if (min_on_ms > cfg->window_ms) {
        min_on_ms = cfg->window_ms;
    }

    state->window_elapsed_ms += dt_ms;
    if (!state->window_started || state->window_elapsed_ms >= cfg->window_ms) {
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
