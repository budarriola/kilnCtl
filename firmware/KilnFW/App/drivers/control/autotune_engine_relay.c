#include "autotune_engine_internal.h"

/* RELAY method: the bang-bang law, the relay fit, autotune_engine_run_relay().
 * See autotune_engine_internal.h's top comment for the full five-way split
 * this file is one piece of. */

/* One tick of the bang-bang relay law. Returns the duty this tick wants and
 * reports, through *edge_on_to_off, the moment the relay switched from its
 * high branch to its low one.
 *
 * That single edge is the cycle boundary for everything downstream: it is the
 * instant the measurement first crossed the TOP of the switching band, so
 * consecutive edges bound exactly one period, and starting the recorded trace
 * on one means the trace begins at a known point in the cycle rather than
 * wherever the approach happened to end.
 *
 * The law is evaluated every tick (1 Hz) rather than once per recorded sample
 * (10 s): the switch instants are what set Tu, and quantising them to the
 * trace's sample period would quantise the measured period along with them.
 *
 * Control uses the CALIBRATED reading, the same way profile_executor's loop
 * does -- the operator's setpoint is in the units they read off the page.
 * (Guards separately get the raw value; see the guard input below.)
 *
 * One consequence of centring on 0.5 rather than swinging to full-off is worth
 * stating plainly instead of leaving to be discovered: the low branch is duty
 * 0.15, not 0, so guard 3 (runaway with heat commanded OFF -- the welded-relay
 * check) never opens its window during cycling, because heat is never
 * commanded off. Nothing about the guard is disabled or reconfigured; it
 * simply has no off-period to observe. A welded contact during a relay test is
 * caught instead by guard 5's absolute ceiling and guard 4's drift check, both
 * of which are armed and now have a real setpoint to judge against. The
 * alternative -- a full-off low branch -- would arm guard 3 but would also make
 * it fire on the ordinary case, since a kiln's temperature keeps climbing for
 * minutes after heat is cut and guard 3 trips on exactly that after 120s.
 *
 * Must be called with s_at.lock held. */
float relay_law_tick(bool sensor_ok, float meas_c, bool *edge_on_to_off)
{
    *edge_on_to_off = false;
    /* A bad reading freezes the branch rather than switching on a guess. The
     * caller zeroes the commanded duty for this tick anyway, and guard 6 ends
     * the run after its own debounce if the sensor really is gone; what must
     * not happen is a dropout being counted as a cycle. */
    if (sensor_ok) {
        bool was_on = s_at.relay_on;
        if (meas_c < s_at.relay_setpoint_c - s_at.relay_h) {
            s_at.relay_on = true;
        } else if (meas_c > s_at.relay_setpoint_c + s_at.relay_h) {
            s_at.relay_on = false;
        } /* inside the band: hold the branch -- this is the hysteresis */
        *edge_on_to_off = was_on && !s_at.relay_on;
    }
    /* u0 +/- d, never clamped: run_relay() has already refused any d that
     * would push a branch outside [0, 1], because a clamped branch would make
     * the real half-amplitude smaller than the d handed to the fit and inflate
     * Ku by exactly that ratio. */
    return AUTOTUNE_RELAY_CENTER_DUTY + (s_at.relay_on ? s_at.relay_d : -s_at.relay_d);
}

/* End of a relay run: fit (Ku, Tu) and propose gains, or abort.
 *
 * The asymmetry with autotune_finalize_fit() is the point of the whole method. A step
 * test produces a plant MODEL; a relay test produces one point of the
 * frequency response and nothing else -- no K, no tau, no L. So there is no
 * model to propose here, no ramp-ceiling estimate to derive (that needs tau),
 * and no coupling-matrix row to fill.
 *
 * Must be called with s_at.lock held. */
void finalize_relay_fit(void)
{
    if (s_at.trace_count == 0) {
        /* Distinguished from the allocation failure below because they mean
         * completely different things to whoever reads the abort reason: an
         * empty trace is "the sensor never gave us a usable reading", not
         * "the board ran out of memory". */
        abort_locked("relay test recorded no samples -- no usable readings during cycling");
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    autotune_sample_t *scratch = autotune_unpack_zone_trace(s_at.zone_index, s_at.trace_count);
    if (!scratch) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "out of memory unpacking a %u-sample trace", (unsigned)s_at.trace_count);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    s_at.relay = pid_autotune_fit_relay(scratch, s_at.trace_count, s_at.relay_d, s_at.relay_h);
    free(scratch);

    if (!s_at.relay.valid) {
        /* REJECTED, not approximated. pid_autotune_fit_relay() distinguishes
         * "never oscillated", "cycles still growing/decaying" and "amplitude
         * never escaped the hysteresis band", and every one of those means the
         * plant never reached a limit cycle -- so there is no frequency-response
         * point to have measured and nothing legitimate to propose. Proposing
         * gains from a non-limit-cycle would be inventing a Ku, and the whole
         * value of this method is that Ku came from the kiln. */
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "relay fit rejected: %s", s_at.relay.invalid_reason);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }

    s_at.proposed_gains = pid_autotune_tune_from_relay(&s_at.relay, s_at.relay_rule);
    force_relays_off();
    s_at.state = AUTOTUNE_ENGINE_DONE;
    ESP_LOGI(AT_TAG,
             "autotune zone %u relay done: Ku=%.4f Tu=%.0fs a=%.2fC over %d cycles -> Kp=%.5f Ki=%.5f Kd=%.5f",
             s_at.zone_index, (double)s_at.relay.ku, (double)s_at.relay.tu_s, (double)s_at.relay.amplitude_c,
             s_at.relay.cycles_used, (double)s_at.proposed_gains.kp, (double)s_at.proposed_gains.ki,
             (double)s_at.proposed_gains.kd);
}

bool autotune_engine_run_relay(uint8_t zone_index, float setpoint_c, float relay_d, float hysteresis_c,
                               autotune_rule_t rule, char *err_msg, size_t err_cap)
{
    /* <= 0 means "use the default"; anything else is range-checked. The
     * defaults and their reasoning live in autotune_engine.h next to the
     * constants, not duplicated here. */
    float d = (relay_d > 0.0f) ? relay_d : AUTOTUNE_RELAY_DEFAULT_D;
    float h = (hysteresis_c > 0.0f) ? hysteresis_c : AUTOTUNE_RELAY_DEFAULT_H_C;

    if (d > AUTOTUNE_RELAY_MAX_D) {
        /* Not clamped, refused: silently shrinking d would leave the fit
         * dividing by a d the relay never actually drove, and Ku is directly
         * proportional to it. */
        if (err_msg) snprintf(err_msg, err_cap, "relay amplitude d must be in (0, %.2f]", (double)AUTOTUNE_RELAY_MAX_D);
        return false;
    }
    if (h < AUTOTUNE_RELAY_MIN_H_C || h > AUTOTUNE_RELAY_MAX_H_C) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "hysteresis h must be in [%.1f, %.1f] degC (half-width of the band)",
                     (double)AUTOTUNE_RELAY_MIN_H_C, (double)AUTOTUNE_RELAY_MAX_H_C);
        }
        return false;
    }
    if (rule != AUTOTUNE_RULE_ZIEGLER_NICHOLS && rule != AUTOTUNE_RULE_TYREUS_LUYBEN) {
        /* pid_autotune_tune_from_relay() returns all-zero gains for SIMC
         * rather than erroring, so this has to be caught here or the operator
         * would sit through a multi-hour oscillation to be handed kp=ki=kd=0. */
        if (err_msg) snprintf(err_msg, err_cap, "relay tuning rule must be Tyreus-Luyben or Ziegler-Nichols");
        return false;
    }

    /* The setpoint is checked against this zone's OWN guard limits before
     * anything is driven. A test whose oscillation is designed to sit where
     * guard 5 trips is not a test, it is a scheduled abort several hours from
     * now with the kiln at temperature in the meantime. */
    float max_temp_c = 0.0f, min_temp_c = -20.0f;
    zones_config_get_temp_limits(zone_index, &max_temp_c, &min_temp_c);
    if (!(max_temp_c > 0.0f)) {
        /* thermal_guard treats max_temp_c == 0 as "no ceiling". That is
         * survivable for a step test the operator watches climb; it is not
         * something to hand a deliberate oscillation at temperature. */
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone %u has no max temperature set -- set one before running a relay test", zone_index);
        }
        return false;
    }
    if (!(setpoint_c > 0.0f)) {
        if (err_msg) snprintf(err_msg, err_cap, "relay test requires an oscillation setpoint (degC)");
        return false;
    }
    /* AUTOTUNE_RELAY_SETPOINT_HEADROOM_C (50C) either side is the right
     * margin whenever the zone's span leaves room for it, but a zone can be
     * legitimately commissioned with a narrow span (this rig: max 80C, min
     * 0C -- a 80C span against a 50C-each-side headroom demands 100C of
     * headroom alone, which is an EMPTY window: every setpoint was refused,
     * one side or the other, and relay identification could never run at
     * all). Scale the headroom down to a quarter of the span when the full
     * 50C does not fit, so the window degrades gracefully instead of
     * vanishing. A span so narrow that even the scaled-down headroom leaves
     * nothing (window would invert) is refused once, with the computed
     * window stated -- never the old two mutually exclusive messages, which
     * told the operator nothing about what *would* have been accepted. */
    float span = max_temp_c - min_temp_c;
    float headroom = AUTOTUNE_RELAY_SETPOINT_HEADROOM_C;
    /* <= not < : at span exactly 2*HEADROOM, window_lo == window_hi below
     * (an empty-but-not-inverted window), which the window_lo >= window_hi
     * check just below refuses anyway -- but only once the fallback headroom
     * has actually been applied. A strict `<` here left span == 100.0 (with
     * the default 50C headroom) using the UN-scaled full headroom, producing
     * that exact degenerate window, refused, while a span of 99.9 used the
     * scaled-down quarter-span headroom and got a real, usable window -- a
     * WIDER span refused where a narrower one worked. */
    if (span <= 2.0f * AUTOTUNE_RELAY_SETPOINT_HEADROOM_C) {
        headroom = span * 0.25f;
    }
    float window_lo = min_temp_c + headroom;
    float window_hi = max_temp_c - headroom;
    if (window_lo >= window_hi) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone %u's span (%.0fC floor to %.0fC limit) is too narrow for a relay test -- "
                     "no setpoint window exists",
                     zone_index, (double)min_temp_c, (double)max_temp_c);
        }
        return false;
    }
    if (setpoint_c < window_lo || setpoint_c > window_hi) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "relay setpoint must be between %.0fC and %.0fC for zone %u (floor %.0fC, limit %.0fC, "
                     "%.0fC headroom each side)",
                     (double)window_lo, (double)window_hi, zone_index, (double)min_temp_c, (double)max_temp_c,
                     (double)headroom);
        }
        return false;
    }

    /* REVIEW 2026-09-02: the window above is derived from `headroom` alone
     * and knows nothing about `h`, the hysteresis half-width this run will
     * actually oscillate over. Before 17e67ee that was harmless -- the
     * headroom was a fixed 50C and AUTOTUNE_RELAY_MAX_H_C is 20C, so the
     * band could never reach a guard limit. With the span-proportional
     * fallback the headroom can now be SMALLER than a legal h (this rig:
     * span 80C -> headroom 20C, and h may be up to 20C), so a setpoint at
     * the top of the window with a large h puts the band's upper edge AT or
     * ABOVE max_temp_c -- exactly the "a test whose oscillation is designed
     * to sit where guard 5 trips" case the comment at the top of this block
     * says must never be accepted. Check the band itself, independently of
     * how the window was computed, so the invariant holds for every future
     * headroom formula. Strict inequalities: guard 5 trips AT max_temp_c,
     * and the band edge is where the relay switches, not where the
     * temperature stops climbing. */
    if (setpoint_c + h >= max_temp_c || setpoint_c - h <= min_temp_c) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "relay band %.1fC..%.1fC (setpoint %.1fC +/- %.1fC) reaches zone %u's guard limits "
                     "(floor %.0fC, limit %.0fC) -- lower the hysteresis or move the setpoint",
                     (double)(setpoint_c - h), (double)(setpoint_c + h), (double)setpoint_c, (double)h,
                     zone_index, (double)min_temp_c, (double)max_temp_c);
        }
        return false;
    }

    if (!autotune_begin_run_locked(zone_index, err_msg, err_cap)) {
        return false;
    }

    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.step_duty = 0.0f; /* no step is ever applied on this path */
    s_at.relay_setpoint_c = setpoint_c;
    s_at.relay_d = d;
    s_at.relay_h = h;
    s_at.relay_rule = rule;
    /* Same override as the step-test path (AUTOTUNE_PROGRESS_DUTY_MIN_FOR_
     * STEP_TEST's own comment), now that the guard input feeds want_duty
     * (relay_law_tick()'s pre-PWM branch value) instead of the post-PWM
     * want_relay_on-gated duty -- see that construction's own comment. A
     * relay run's low branch is AUTOTUNE_RELAY_CENTER_DUTY - d, 0.15 at the
     * default d=0.35 -- below thermal_guard.c's stock 0.5 progress_duty_min,
     * so without this override guard 1/2's window would still degate every
     * time the branch dips low, even though commanded_duty no longer drops
     * to 0 on every PWM sub-cycle within a branch. Guard 3 is unaffected:
     * it keys off commanded_duty <= 0, not this threshold, so a low branch
     * that stays strictly positive (the normal case) still correctly never
     * opens guard 3's window -- see this run's guard-input comment. */
    s_at.guard_cfg.progress_duty_min = AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST;
    /* Start on the high branch and let the law correct it on the first tick
     * that has a reading. Starting "on" is the right guess for the usual case
     * (a cold kiln heading up to the setpoint) and costs nothing in the other:
     * if the zone is already above the band the very next tick switches it
     * off, and the approach phase simply waits for the first full high->low
     * edge, which it would have had to do anyway. */
    s_at.relay_on = true;
    s_at.state = AUTOTUNE_ENGINE_RELAY_APPROACH;
    xSemaphoreGive(s_at.lock);

    ESP_LOGW(AT_TAG,
             "autotune zone %u starting RELAY test: oscillating around %.1fC, duty %.2f/%.2f, band +/-%.1fC, "
             "%u cycles wanted -- this deliberately cycles the kiln at temperature",
             zone_index, (double)setpoint_c, (double)(AUTOTUNE_RELAY_CENTER_DUTY + d),
             (double)(AUTOTUNE_RELAY_CENTER_DUTY - d), (double)h, (unsigned)AUTOTUNE_RELAY_TARGET_CYCLES);
    return true;
}
