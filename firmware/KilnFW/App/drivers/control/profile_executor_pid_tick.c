/* PID-family per-zone tick and its fuzzy-gain/bumpless-transfer plumbing --
 * split out of profile_executor.c (2026-09-01, "files over 1500 lines
 * should be broken up where it makes sense"). See
 * profile_executor_internal.h's own doc comment for the full multi-way
 * split this is one piece of. exec_threshold()/the EXEC_*_C() macros read
 * per-zone executor thresholds (bang-bang hysteresis, cooling margin/hold,
 * ramp-lock band) that executor_task_entry() (profile_executor.c) also
 * consumes directly via those macros. */

#include "profile_executor_internal.h"

#include <math.h>

#include "pid_fuzzy.h"
#include "zones_config_accessors.h"
#include "zones_config_json.h" /* zones_config_get_error_band_c()/_rate_band_c_per_s() */

/* PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md option
 * (b): declared locally, same convention profile_executor_feedforward.c
 * already uses for zones_config_get_ease_off_window_mult() -- this pass's
 * touched-files list does not include zones_http.h, and the real
 * declaration lives in zones_config_json.h (keep this in sync with it). */
bool zones_config_get_approach_rate_cap_c_per_hr(uint8_t zone_index, float *out_cap_c_per_hr);

/* Resolves what THIS zone's control loop should treat as its own commanded
 * setpoint this tick: the shared s_exec.target_c when uncapped (this zone's
 * approach_rate_cap_c_per_hr reads 0 -- the default, and every zone before
 * this field existed), or z->effective_target_c (profile_executor.c's
 * per-tick rate-limited value, computed once per tick ahead of the control-
 * mode pass) when capped. Re-checking the cap here rather than trusting
 * z->effective_target_c unconditionally means a caller that never runs
 * profile_executor.c's own per-tick cap-update loop (every existing
 * host test that calls pid_family_zone_tick()/zone_feedforward() directly
 * with a hand-built zone_runtime_t, effective_target_c left at its
 * zero-initialized default) still gets EXACTLY today's behaviour for the
 * overwhelmingly common uncapped case, with no test changes required --
 * only a test that deliberately configures a non-zero cap needs to also
 * seed effective_target_c, which is exactly the scenario a new cap-specific
 * test does deliberately. */
float zone_commanded_setpoint_c(const zone_runtime_t *z, uint8_t zi)
{
    float cap_c_per_hr = 0.0f;
    (void)zones_config_get_approach_rate_cap_c_per_hr(zi, &cap_c_per_hr);
    return (cap_c_per_hr > 0.0f) ? z->effective_target_c : s_exec.target_c;
}

float exec_threshold(uint8_t zone_index, int which)
{
    float bb = 0.0f, cool_margin = 0.0f, cool_hold = 0.0f, ramp_lock = 0.0f;
    (void)zones_config_get_executor_thresholds(zone_index, &bb, &cool_margin, &cool_hold, &ramp_lock);
    switch (which) {
    case 0: return (bb > 0.0f) ? bb : PROFILE_EXECUTOR_HYSTERESIS_C;
    case 1: return (cool_margin > 0.0f) ? cool_margin : PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C;
    case 2: return (cool_hold > 0.0f) ? cool_hold : PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S;
    default: return (ramp_lock > 0.0f) ? ramp_lock : PROFILE_EXECUTOR_RAMP_LOCK_BAND_C;
    }
}
#define EXEC_BANGBANG_HYSTERESIS_C(zi) exec_threshold((zi), 0)
#define EXEC_COOLING_MARGIN_C(zi)      exec_threshold((zi), 1)
#define EXEC_COOLING_HOLD_S(zi)        exec_threshold((zi), 2)
#define EXEC_RAMP_LOCK_BAND_C(zi)      exec_threshold((zi), 3)

/* Shared PID-family per-zone tick body -- everything downstream of "which
 * pid_cfg_t to run this tick under" (PID_EXPANSION_PLAN.md Phase 3 wiring):
 * the pid_update_terms() call itself, TODO.md 6A.2's cooling-limited
 * diagnostic, and 6A.5's load-cap credit payback. ZONE_CONTROL_MODE_PID
 * calls this with cfg == &z->pid_cfg unchanged (so its behavior is
 * byte-for-byte what it always was -- this refactor changes nothing on that
 * path, only where the code lives). ZONE_CONTROL_MODE_PID_FUZZY calls it
 * with a per-tick fuzzy-adjusted copy. Extracted once instead of duplicated,
 * per PID_CONTROL.md's "each zone with its own instance of the three pure
 * modules" -- fuzzy is a variant of the PID module, not a second copy of
 * this several-hundred-line body that could drift from the original. */
float pid_family_zone_tick(zone_runtime_t *z, uint8_t zi, const pid_cfg_t *cfg,
                                  bool sensor_ok_zi, float dt_s, uint32_t dt_ms,
                                  bool *out_want_relay_on)
{
    float duty = 0.0f;
    if (sensor_ok_zi) {
        /* TODO.md 6A.2's feedforward. 0.0f -- byte-for-byte the behaviour of
         * every firing before this -- for any zone with no identified
         * model, which is every zone that has never been autotuned. Passed
         * IN to the controller so the clamp, the anti-windup
         * conditional-integration test and the term breakdown all see the
         * same number the element is driven from; the value handed over
         * here is what /api/control reports as "ff", so the operator can
         * read the P/I/D/FF split and see how much of the duty is the model
         * and how much is the loop correcting it. */
        /* Terminal ease-off (PID_EXPANSION_PLAN.md sec 3.1, validated
         * sim_calibration.md sec 5): taper the feedforward's rate input only
         * as this zone's own ramp approaches the segment's target -- see
         * zone_taper_climb_rate()'s doc comment for the formula/window and
         * why tapering only the feedforward, never s_exec.target_c, bounds
         * the worst case to ordinary PID tracking lag.
         *
         * Gated on s_exec.dwelling, not merely target_rate_c_per_s != 0: a
         * ramp-lock stall (profile_executor.c's segment-stepping block)
         * zeros target_rate_c_per_s WITHOUT setting dwelling, for a lagging
         * zone waiting on its neighbours to catch up. zone_taper_climb_rate()
         * already returns 0 for a 0 input rate regardless of this gate, so a
         * ramp-lock stall cannot be pushed into the taper's distance/rate
         * division either way -- but gating on dwelling here (rather than
         * relying on that zero-rate coincidence) is the same discipline a
         * previous change in this file had to adopt for exactly this
         * ramp-lock-without-dwelling shape, and keeps the taper's *intent* --
         * "only while an active ramp is actually advancing" -- explicit
         * rather than implicit in one arm's arithmetic. */
        float ff_rate = s_exec.target_rate_c_per_s;
        /* ROADMAP.md M15 B4: pretaper rate captured before the taper below
         * can touch it -- read-only, see zone_duty_breakdown_t's comment. */
        z->duty_breakdown.ff_rate_pretaper_c_per_s = ff_rate;
        if (!s_exec.dwelling && ff_rate != 0.0f) {
            const profile_segment_t *seg = &s_exec.profile.segments[s_exec.segment_index];
            /* PID_EXPANSION_PLAN.md sec 3.6d: this zone's own (possibly
             * rate-capped) commanded setpoint, not s_exec.target_c
             * unconditionally -- see zone_commanded_setpoint_c()'s own doc
             * comment. Uncapped, bit-identical to before. */
            ff_rate = zone_taper_climb_rate(z, zi, zone_commanded_setpoint_c(z, zi), ff_rate, seg->target_c);
        }
        z->duty_breakdown.ff_rate_posttaper_c_per_s = ff_rate;
        /* Gains actually in force this tick -- cfg is z->pid_cfg unchanged
         * for plain PID, or pid_fuzzy_prepare_gains()'s adjusted copy for
         * PID_FUZZY (see this function's own top-of-file doc comment). */
        z->duty_breakdown.kp_effective = cfg->kp;
        z->duty_breakdown.ki_effective = cfg->ki;
        z->duty_breakdown.kd_effective = cfg->kd;
        float ff_hold = 0.0f;
        /* PID_EXPANSION_PLAN.md sec 3.6d: same swap as zone_taper_climb_
         * rate() just above -- the feedforward's hold term is computed
         * against THIS zone's own commanded setpoint, not the shared
         * destination every zone used to share unconditionally. */
        float u_ff = zone_feedforward(z, zi, zone_commanded_setpoint_c(z, zi), ff_rate, &ff_hold);
        /* Opus review, blocker 2: the coupled system's MEMBERSHIP (which
         * zones are in it, or whether this zone qualifies at all) can change
         * every tick -- heat_blocked alone is refreshed unconditionally each
         * tick by apply_relay() (see its own doc comment), so a flapping
         * interlock or OTA heat-block can flip a neighbour's qualification
         * at tick rate. That steps the WHOLE hold term with nothing to damp
         * it (measured on this file's own tests: up to ~10 duty points in a
         * single tick, landing on every remaining zone at once).
         *
         * Re-seeding here -- reusing seed_bumpless_with_ff(), the SAME
         * bump-transfer mechanism already used for mode transitions and
         * resume (see its own doc comment) -- was chosen over adding a
         * second mechanism (hysteresis/dwell on membership) for two
         * reasons: (1) the reviewer's own instruction to reuse rather than
         * duplicate, and (2) hysteresis only delays the eventual step, it
         * does not remove it, and would need new dwell-timer state on top
         * of the flag this already reads. Re-seeding does not prevent
         * membership from changing -- a flapping interlock still flips the
         * FEEDFORWARD TARGET every tick, which is a separate, pre-existing
         * hazard this fix does not silently paper over (see apply_relay()'s
         * heat_blocked comment) -- but it guarantees the transition itself
         * never shows up as a COMMANDED DUTY step: the integral is
         * re-seeded so THIS tick's pid_update_terms() call below reproduces
         * z->duty (the duty already being commanded) to within the same
         * anti-windup tolerance every other bumpless seed in this file
         * already accepts, and subsequent ticks converge toward the new
         * target smoothly rather than jumping to it. z->duty, not
         * z->last_pid_terms.ff, is deliberately used as u_desired: it is
         * the actual physical thing that must not step. */
        if (z->ff_membership_changed) {
            seed_bumpless_with_ff(z, zi, z->duty);
            /* Same bump-transfer bookkeeping as the other reseed paths
             * (reload_zone_config()'s gain/model reloads, resume()) --
             * without this, pid_fuzzy_prepare_gains()'s next tick rescales
             * the integral seed_bumpless_with_ff() just set against a stale
             * effective-Ki ratio instead of treating the reseed as the
             * no-op it's supposed to be. */
            z->fuzzy_prev_effective_ki = z->pid_cfg.ki;
        }
        /* PID_EXPANSION_PLAN.md sec 3.6d: the PID error term is driven from
         * this zone's own commanded setpoint too, so a capped zone's own
         * loop chases its own (slower) commanded setpoint rather than the
         * shared, faster-moving destination -- otherwise the P/I/D terms
         * would fight the feedforward's own deliberately tapered rate
         * above. Uncapped, this is bit-identical to s_exec.target_c. */
        duty = pid_update_terms(&z->pid_state, cfg, zone_commanded_setpoint_c(z, zi), z->actual_c, dt_s,
                                u_ff, ff_hold, &z->last_pid_terms);
    }
    /* ROADMAP.md M15 B4: Stage B of the duty breakdown -- read-only. duty
     * already reflects both branches above (pid_update_terms()'s clamped
     * result, or the 0.0f no-sensor fallback the `duty` local was
     * initialized to), so this is accurate for either case without needing
     * its own conditional. */
    z->duty_breakdown.post_clamp_total = duty;
    /* TODO.md 6A.2's cooling-limited diagnostic. Checked against the RAW
     * duty pid_update_terms() just returned, before the load-cap boost
     * below can add anything to it -- a boosted duty is not "the loop asked
     * for heat," it is "another zone's deferred credit landed here," and
     * boost only ever makes duty larger, never masks a genuine 0. */
    /* PID_EXPANSION_PLAN.md sec 3.6d: this zone's own commanded setpoint,
     * not the shared s_exec.target_c unconditionally -- a capped zone
     * intentionally trailing the shared destination is not "cooling
     * limited" relative to a target it was never asked to be at yet.
     * Uncapped, bit-identical to before. */
    if (sensor_ok_zi && duty <= 0.0f &&
        z->actual_c > zone_commanded_setpoint_c(z, zi) + EXEC_COOLING_MARGIN_C(zi)) {
        z->cooling_limited_hold_s += dt_s;
    } else {
        z->cooling_limited_hold_s = 0.0f;
    }
    z->cooling_limited = z->cooling_limited_hold_s >= EXEC_COOLING_HOLD_S(zi);
    /* Pay back any load-cap-deferred on-time as a duty boost -- only
     * actually consumed below if this tick turns out to open a fresh
     * window (heater_output_duty() only reads the duty argument at a
     * window boundary; a boost handed to it mid-window is silently
     * ignored, so crediting it back here unconditionally and only debiting
     * on a real boundary keeps the books exact even if several ticks pass
     * between boundaries). */
    float boosted_duty = duty;
    float credit_ms = 0.0f;
    /* sensor_ok_zi gates this the same as the raw PID compute above:
     * without it, a zone that accrued load-cap credit (TODO.md 6A.5) and
     * then lost its thermocouple would have `duty` correctly held at 0.0f
     * by the `if (sensor_ok_zi)` above, but this block ran unconditionally
     * and could still boost `boosted_duty` up to 1.0f from the credit
     * alone -- commanding full output on a dead sensor, the exact case the
     * BANGBANG branch below explicitly refuses ("want_raw = false; no
     * trustworthy reading -> never command heat"). PID had no equivalent
     * until now.
     *
     * The credit itself is left untouched rather than forfeited: it
     * represents on-time this zone was denied by the load cap, a
     * bookkeeping fact that has nothing to do with whether the
     * thermocouple is currently readable. Discarding it would
     * double-penalize the zone -- once for losing its window to the cap,
     * again for a sensor fault that is very likely transient (TODO.md
     * 6A.3's SPI retry/debounce). Leaving deferred_on_ms as-is means the
     * credit is simply not spent this tick and is still there to pay back
     * once the sensor (and therefore sensor_ok_zi) recovers. */
    if (sensor_ok_zi && z->deferred_on_ms > 0.0f && z->heater_cfg.window_ms > 0) {
        float window_ms_f = (float)z->heater_cfg.window_ms;
        credit_ms = z->deferred_on_ms;
        float max_credit_ms = (1.0f - boosted_duty) * window_ms_f;
        if (credit_ms > max_credit_ms) credit_ms = max_credit_ms;
        if (credit_ms < 0.0f) credit_ms = 0.0f;
        boosted_duty += credit_ms / window_ms_f;
    }
    /* ROADMAP.md M15 B4: Stage C/final -- read-only. boosted_duty is fully
     * settled at this point (nothing below changes it before it's handed to
     * heater_output_duty()). */
    z->duty_breakdown.load_cap_boost = boosted_duty - z->duty_breakdown.post_clamp_total;
    z->duty_breakdown.final_commanded = boosted_duty;
    uint32_t elapsed_before = z->heater_state.window_elapsed_ms;
    bool was_started = z->heater_state.window_started;
    *out_want_relay_on = heater_output_duty(&z->heater_state, &z->heater_cfg, boosted_duty, dt_ms);
    /* A fresh window opened this tick iff window_elapsed_ms got reset to 0
     * -- the only place heater_output_duty() sets it to exactly 0 is the
     * new-window branch (see its comment); 1Hz ticks against a >=1s window
     * make an accumulation-only 0 practically impossible. Only then did
     * boosted_duty actually get baked into on_ms_this_window, so only then
     * is the credit actually spent. */
    if (credit_ms > 0.0f && z->heater_state.window_elapsed_ms == 0 &&
        (!was_started || elapsed_before > 0)) {
        z->deferred_on_ms -= credit_ms;
        if (z->deferred_on_ms < 0.0f) z->deferred_on_ms = 0.0f;
    }
    return duty;
}

/* ZONE_CONTROL_MODE_PID_FUZZY's per-tick gain computation
 * (PID_EXPANSION_PLAN.md Phase 3, "two wiring hazards found in review").
 *
 * Hazard 1 (error_rate_c_per_s producer): pid.c's own d_filtered is
 * derivative-on-measurement, low-pass filtered through d_filter_tau_s --
 * exactly the signal PID_EXPANSION_PLAN.md says to reuse rather than have
 * this file invent a raw per-tick finite difference (which would feed the
 * rule table unfiltered ADC noise and chatter gains cell-to-cell). Since
 * error = setpoint - measurement, d(error)/dt = -d(measurement)/dt whenever
 * the setpoint is locally constant -- and pid.c's own d_filtered is defined
 * as exactly -d(measurement)/dt (see pid_update_terms()'s raw_d), so
 * z->pid_state.d_filtered IS error_rate_c_per_s under that same
 * "setpoint locally constant" assumption pid.c already makes for its own D
 * term. Read BEFORE this tick's pid_update_terms() call runs (so it reflects
 * up through last tick, one tick of lag on an already ~30s-time-constant
 * filter -- immaterial) rather than after, because gains have to be chosen
 * before the call that uses them.
 *
 * The dSP/dt decision, stated explicitly per the plan's requirement: this
 * deliberately does NOT add the setpoint's own ramp rate
 * (s_exec.target_rate_c_per_s) into error_rate_c_per_s. During a profile
 * ramp the setpoint is moving at a known, constant, non-disturbance rate;
 * folding it in would make a perfectly ordinary firing register as "rising"
 * or "falling" on the rate axis for the entire ramp, for a reason that has
 * nothing to do with plant behavior the fuzzy layer should be reacting to.
 * pid.c's own derivative-on-measurement choice (not derivative-on-error) is
 * the same judgment call for the same reason -- a moving setpoint must not
 * by itself look like a disturbance -- and this reuses that precedent rather
 * than re-deriving a different answer for the same question. The
 * measurement's own filtered rate can still be large during a
 * well-tracked ramp (the actual temperature IS climbing at close to the
 * ramp rate) -- that is real, physical, filtered signal, not noise, and it
 * is exactly what hazard 2's rescaled RATE_BAND_C_PER_S below is sized to
 * treat as unremarkable rather than "large."
 *
 * Hazard 3 (bump transfer on a Ki move): the rule table can legitimately
 * land on a different cell tick to tick as error/error_rate drift, so the
 * effective Ki pid_fuzzy_adjust() returns can change every tick, and
 * pid_state.integral persists across that change -- i_term = ki*integral
 * would then step discontinuously the instant Ki moves. Rather than route
 * every such move through pid_seed_bumpless() (which re-derives the
 * integral from a *desired output*, appropriate for a deliberate
 * discontinuity like a mode change, not for a per-tick nudge), this rescales
 * the integral inversely so ki_old*integral == ki_new*integral' -- the I
 * term's actual contribution to duty is unchanged by the rescale itself,
 * only by the (deliberate, bounded) change in Ki. z->fuzzy_prev_effective_ki
 * tracks "effective Ki last tick" across the mode-change/reseed paths in
 * reload_zone_config()/resume() too, so this stays correct after any of
 * those discontinuities as well. */
void pid_fuzzy_prepare_gains(zone_runtime_t *z, uint8_t zi, pid_cfg_t *out_cfg)
{
    *out_cfg = z->pid_cfg; /* d_filter_tau_s/b/pid_range_c untouched -- only kp/ki/kd move */

    /* docs/FUZZY_CONTROLLER_PLAN.md finding (D), fixed 2026-09-11: this used
     * to read the shared s_exec.target_c unconditionally while
     * pid_family_zone_tick()'s own pid_update_terms()/zone_feedforward()
     * calls (this file, above) read zone_commanded_setpoint_c(z, zi) -- the
     * "paired input left shared" class this repo has hit four times before
     * (project_paired_input_left_shared.md and its siblings): a per-zone
     * approach-rate cap was added (PER_ZONE_TARGET_DESIGN_STUDY.md option (b))
     * and this consumer was left reading the pre-cap shared value. That
     * design study's own text says the gain-scheduling axis was deliberately
     * left unswapped -- but "deliberately" there meant "not costed", not
     * "verified safe": a capped zone's REAL tracking error (what the P/I/D
     * terms actually chase) is `zone_commanded_setpoint_c(z, zi) - actual_c`,
     * so scheduling gains off the shared, faster-moving destination instead
     * picks a rule-table cell for an error the loop is not actually seeing --
     * exactly backwards for a gain scheduler. Fixed by calling the same
     * helper pid_family_zone_tick() already uses, rather than copying its
     * result, so there is one owning function for "this zone's commanded
     * setpoint this tick" and no second copy that can drift again. Inert on
     * every zone today (approach_rate_cap_c_per_hr reads 0.0 live, confirmed
     * via control_get_zones over the kilnctrl MCP 2026-09-11), so this changes
     * no live behaviour -- it only fires once a zone is given a non-zero
     * cap, which is the point of fixing it now rather than after. */
    float error_c = zone_commanded_setpoint_c(z, zi) - z->actual_c;
    float error_rate_c_per_s = z->pid_state.d_filtered; /* hazard 1, see comment above */

    float strength_pct_f = 0.0f;
    (void)zones_config_get_fuzzy_strength_pct(zi, &strength_pct_f);
    /* Defence-in-depth on the duty path: zones_http.c validates this on load
     * so a non-finite value should never reach here, but if one ever did,
     * both clamp comparisons below are false for NaN and (uint8_t)(NaN+0.5f)
     * is undefined behaviour -- guard it explicitly rather than trust the
     * loader stayed the only path in. */
    uint8_t strength_pct = (!isfinite(strength_pct_f)) ? 0
                          : (strength_pct_f < 0.0f) ? 0
                          : (strength_pct_f > 100.0f) ? 100
                          : (uint8_t)(strength_pct_f + 0.5f);

    /* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): the
     * membership-band widths are now per-zone config, not pid_fuzzy.c's own
     * compile-time constants -- resolved here, the same tick this zone's
     * fuzzy strength is resolved.
     *
     * The (void)-discarded bool return is deliberately safe, not a
     * "consumer without producer" gap: these getters can only return false
     * for zi >= MAX31856_CHANNEL_COUNT (zi is this loop's own zone index,
     * always in range by construction, never user input), and the 0.0f the
     * locals are pre-initialized to on that unreachable path is NOT a raw
     * unhandled zero -- it is pid_fuzzy_adjust()'s OWN documented "non-
     * positive band" sentinel (pid_fuzzy.c's `> 0.0f` check), which that
     * function resolves to ERROR_BAND_C_DEFAULT/RATE_BAND_C_PER_S_DEFAULT
     * (20.0/0.5) internally before touching the membership math. So even a
     * hypothetical failed lookup here still reaches pid_fuzzy_adjust() with
     * the documented firmware default, by the same mechanism the accessors
     * themselves use, never with an unvalidated 0. */
    float error_band_c = 0.0f, rate_band_c_per_s = 0.0f;
    (void)zones_config_get_error_band_c(zi, &error_band_c);
    (void)zones_config_get_rate_band_c_per_s(zi, &rate_band_c_per_s);

    float adj_kp = z->pid_cfg.kp, adj_ki = z->pid_cfg.ki, adj_kd = z->pid_cfg.kd;
    pid_fuzzy_adjust(error_c, error_rate_c_per_s, error_band_c, rate_band_c_per_s,
                     z->pid_cfg.kp, z->pid_cfg.ki, z->pid_cfg.kd,
                     strength_pct, &adj_kp, &adj_ki, &adj_kd);

    /* Hazard 3's bump transfer, via pid.c's own pid_rescale_integral_for_new_ki()
     * (see its header comment for why this is the right tool, not
     * pid_seed_bumpless()). z->fuzzy_prev_effective_ki starting at 0.0f on a
     * cold start/mode change/reseed makes this a no-op right after any of
     * those -- pid_reset()/pid_seed_bumpless() already gave the integral a
     * correct starting value for THAT discontinuity, so rescaling again on
     * top of a value just deliberately set would be wrong, not extra-safe. */
    pid_rescale_integral_for_new_ki(&z->pid_state, z->fuzzy_prev_effective_ki, adj_ki);
    z->fuzzy_prev_effective_ki = adj_ki;

    out_cfg->kp = adj_kp;
    out_cfg->ki = adj_ki;
    out_cfg->kd = adj_kd;
}

/* How long the PC link may stay silent before a running firing is aborted.
 * Operator-settable since v8 (one global field, not per-zone -- the link is
 * one wire to one PC); 0 keeps the constant this was before. */
