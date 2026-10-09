#include "autotune_engine_internal.h"

/* STEP method: settle/onset detection, the FOPDT fit and its guards
 * (autotune_finalize_fit()), target-mode probe handling, pre-start thermal
 * readiness. See autotune_engine_internal.h's top comment for the full
 * five-way split this file is one piece of. */

/* The minimum rise (degC) a step test's zone must show above its own
 * baseline before it is trustworthy -- shared by autotune_finalize_fit()'s (B)
 * minimum-excursion refusal (the fitted gain's implied total rise) AND
 * the live "has this element proven it heats" check that gates guard 1's
 * relaxation during STEPPING (see step_element_proven's comment). One
 * threshold, two consumers -- deliberately the SAME number: if it's not
 * enough rise to trust a finished fit, it's not enough rise to have proven
 * the element yet either. Factored out of autotune_finalize_fit() (where this used
 * to be inlined) rather than duplicated. ZERO-SEMANTICS TRAP: max_temp_c
 * == 0 means the ceiling guard is DISABLED, not "the ceiling is zero
 * degrees" -- see AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN's own comment above
 * for the full reasoning behind the fraction-of-headroom vs. no-ceiling
 * split below. */
/* probe_k_rough: the CURRENT run's rough gain estimate (0 if unavailable --
 * see AUTOTUNE_REFERENCE_K_C_PER_DUTY's own comment for the fallback this
 * triggers). Callers that have no run-specific plant estimate to offer
 * (check_thermal_readiness_locked(), judging OTHER zones pre-run) pass 0.0f
 * and get the original hand-tuned constants back, unscaled -- exactly the
 * old behavior. */
static float autotune_min_rise_c(float baseline_c, float configured_max_temp_c, float probe_k_rough)
{
    float floor_c = autotune_scale_threshold_c(AUTOTUNE_MIN_RISE_FLOOR_C, probe_k_rough);
    if (configured_max_temp_c > 0.0f) {
        float headroom_c = configured_max_temp_c - baseline_c;
        float scaled = AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN * headroom_c;
        return (scaled > floor_c) ? scaled : floor_c;
    }
    return autotune_scale_threshold_c(AUTOTUNE_MIN_RISE_NO_CEILING_C, probe_k_rough);
}

/* Review blocker 4: guard 1's expected-rise bar (thermal_guard.c: rate_cfg *
 * elapsed_min) is completely duty-independent, while achievable rise on a
 * linear plant is proportional to commanded duty. Combined with review
 * finding 3's AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST (0.01 -- arming
 * guards 1/2 at any commanded duty > 0, not just >= 0.5), this asserted
 * that a 1% duty must rise as fast as a 100% duty: at the FIXED 0.15 probe
 * duty, guard 1's first 300s window needs K*d*(1-e^(-300/285)) >=
 * rate_cfg*5min = 2.5C, i.e. K >= 25.6 -- a perfectly healthy zone with
 * K = 22 (well within a real kiln's plausible range) false-trips at 300s.
 * Target mode's identify phase is worse: a target only a few degrees above
 * baseline computes a duty near 0.05, whose entire asymptote (K*0.05) can
 * sit BELOW the fixed 2.5C bar -- an immediate, unavoidable false abort
 * regardless of how healthy the element is.
 *
 * Fixed the same way profile_executor.c's own analogous defect was (guard
 * 1's expected rate capped at the commanded ramp rate there, see that
 * file's apply-relays-and-guards loop -- same pattern, different achievable-
 * rate source since a step test has no ramp, only a duty): the expected
 * rate is scaled by commanded duty, `rate_cfg * duty`, rather than left as
 * a bare absolute. Physically this is the CORRECT bar, not a relaxation for
 * its own sake -- a dead element still produces ZERO rise at any duty, so a
 * proportionally smaller (but still strictly positive, since step_duty is
 * always > 0 during STEPPING) bar still correctly separates "zero real
 * rise" from "some real rise"; it only removes the FALSE-POSITIVE risk for
 * a healthy, low-K, or low-duty zone that a fixed absolute bar cannot
 * distinguish from a dead one. Deliberately NOT a fixed minimum duty floor
 * on run_to_target()'s computed identify_duty (the OTHER fix this finding's
 * note offered as an alternative) -- scaling the bar already resolves both
 * symptoms described (the fixed-probe-duty K threshold AND target mode's
 * near-zero identify duty) with one mechanism, so a second, overlapping
 * floor was judged unnecessary complexity rather than added safety. */
float autotune_step_guard_sanity_rate(float configured_rate_c_per_min, float step_duty)
{
    float configured = (configured_rate_c_per_min > 0.0f) ? configured_rate_c_per_min : 0.5f;
    return configured * step_duty;
}


void autotune_finalize_fit(void)
{
    float baseline_c = s_at.zone_baseline_c[s_at.zone_index];

    autotune_sample_t *scratch = autotune_unpack_zone_trace(s_at.zone_index, s_at.trace_count);
    if (!scratch) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "out of memory unpacking a %u-sample trace", (unsigned)s_at.trace_count);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    s_at.model = pid_autotune_fit_fopdt(scratch, s_at.trace_count, baseline_c, s_at.step_duty);
    free(scratch);
    if (!s_at.model.valid) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "fit failed: %s", s_at.model.invalid_reason);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    /* Review finding: "no post-hoc check that the identification step
     * landed anywhere near target_c, and status exposes no achieved-vs-
     * requested value." target_achieved_c is what THIS fit says the step
     * actually asymptotes to -- not target_c. Computed for every target-mode
     * fit (not just ones that miss), so an operator always sees the real
     * landing point rather than trusting the requested one. See
     * AUTOTUNE_TARGET_ACHIEVED_WARN_C's own comment for the known
     * probe-baseline-vs-identify-baseline bias this is watching for. */
    if (s_at.target_mode) {
        s_at.target_achieved_c = baseline_c + s_at.model.k_gain_c_per_duty * s_at.step_duty;
        float miss_c = s_at.target_achieved_c - s_at.target_c;
        float target_achieved_warn_c = autotune_scale_threshold_c(AUTOTUNE_TARGET_ACHIEVED_WARN_C, s_at.probe_k_rough);
        if (fabsf(miss_c) >= target_achieved_warn_c) {
            ESP_LOGW(AT_TAG, "autotune zone %u: target-mode identification landed at %.1fC, requested %.1fC "
                          "(miss %.1fC) -- likely the probe-baseline-vs-identify-baseline bias, see "
                          "AUTOTUNE_TARGET_ACHIEVED_WARN_C's comment",
                     s_at.zone_index, (double)s_at.target_achieved_c, (double)s_at.target_c, (double)miss_c);
        } else {
            ESP_LOGI(AT_TAG, "autotune zone %u: target-mode identification landed at %.1fC (requested %.1fC)",
                     s_at.zone_index, (double)s_at.target_achieved_c, (double)s_at.target_c);
        }
    }
    /* (A) fopdt_model_t::settled defaults false; only the honest settle
     * detector in autotune_engine_tick_locked() is allowed to set it true.
     * A fit that reached here via the AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S
     * backstop (s_at.step_settled never set) is therefore marked low-
     * confidence rather than silently accepted as steady state -- logged
     * below, and readable by any caller of autotune_engine_get_status()
     * through status.model.settled. */
    s_at.model.settled = s_at.step_settled;
    if (!s_at.model.settled) {
        ESP_LOGW(AT_TAG, "autotune zone %u: fit accepted from an UNSETTLED trace (ended by the %us max-"
                      "duration backstop, not genuine settling) -- treat K=%.2f tau=%.1fs L=%.1fs as "
                      "low-confidence",
                 s_at.zone_index, (unsigned)AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S,
                 (double)s_at.model.k_gain_c_per_duty, (double)s_at.model.tau_s, (double)s_at.model.dead_time_s);
    }

    /* (B) Minimum-excursion requirement -- threshold scaled to the zone's
     * own span rather than a bare constant, see AUTOTUNE_MIN_RISE_FRACTION_
     * OF_SPAN's comment above for why. Reuses the exact same abort-reason
     * channel every other autotune_finalize_fit() refusal already uses (s_at.
     * abort_reason + state = ABORTED), not a new one. ZERO-SEMANTICS TRAP,
     * same convention as (C) below: max_temp_c == 0 means the guard is
     * DISABLED, not "the ceiling is zero degrees", so the no-ceiling
     * fallback (a large bare constant) applies in that case, not a fraction
     * of a headroom that doesn't exist. */
    float configured_max_temp_c = s_at.guard_cfg.max_temp_c;
    float min_rise_c = autotune_min_rise_c(baseline_c, configured_max_temp_c, s_at.probe_k_rough);
    float observed_rise_c = fabsf(s_at.model.k_gain_c_per_duty * s_at.step_duty);
    if (observed_rise_c < min_rise_c) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        /* Kept short and to the point (both required numbers -- the
         * actual rise and the minimum -- no prose padding): s_at.
         * abort_reason is a fixed 96-byte buffer shared by every refusal
         * in this function, and a wordier 4-substitution version of this
         * message (baseline_c and step_duty included) tripped -Werror=
         * format-truncation on the ESP32 target build -- GCC's worst-case
         * bound for four %f substitutions exceeds 96 bytes even though no
         * real value ever gets close, the same class of problem
         * autotune_engine_accept()'s reasons[] buffer was widened for
         * above. Two substitutions is what the physical-plausibility
         * refusal just below already proved safe (see ITS OWN comment). */
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "fit failed: rise %.2fC is below the %.1fC minimum needed to trust the identification",
                 (double)observed_rise_c, (double)min_rise_c);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }

    /* (C) Physical plausibility -- a static gain small enough that even full
     * duty (u=1) could never reach this zone's own configured ceiling is
     * definitely wrong (a real heater can only get weaker than the fit
     * measured, never stronger), and is exactly the failure mode the old
     * fixed-band settle detector produced: it declared settled mid-ramp,
     * measured a fraction of the true gain, and handed zone_feedforward() a
     * ceiling of ~42-52 degC on a kiln with a triple-digit max_temp_c.
     *
     * 2026-09-01 review fix: implied_max_c is now referenced to AMBIENT
     * (s_at.step_ambient_c, the cold-junction reading captured at the
     * SETTLING->STEPPING transition, or AUTOTUNE_FALLBACK_AMBIENT_C if none
     * was available), not to baseline_c as before. K's physical meaning is
     * "how far above where the zone STARTS COLD it can climb at full duty",
     * and pid_autotune_estimate_max_ramp_c_per_hr() (pid_autotune.c) already
     * uses ambient for the same reason. Referencing baseline_c instead
     * inflates implied_max_c whenever a run starts hot (e.g. re-tuning a
     * zone that is already partway up a firing) by (baseline_c -
     * ambient_c), which makes this check MORE PERMISSIVE exactly when it
     * should not be -- a bad (too-low) fit is more likely to slip through
     * plausible-looking on a hot start than a cold one.
     *
     * ZERO-SEMANTICS TRAP: max_temp_c == 0 means the guard is DISABLED for
     * this zone (thermal_guard.c's convention, mirrored everywhere else in
     * this file -- see the thermal_guard_input_t.setpoint_c fallback
     * further down), NOT "zero degrees is the ceiling". This check must
     * only run when max_temp_c > 0.0f, or every zone that has never had a
     * ceiling configured would have every step test refused.
     *
     * Kept a hard abort rather than downgraded to an override-able warning
     * (unlike the settled-flag case, see autotune_engine_accept()'s own
     * comment): an implied ceiling below a CONFIGURED max_temp_c means
     * either the fit is wrong or the zone's own configuration is wrong, and
     * the fix in the latter case is to correct max_temp_c (a deliberate,
     * infrequent operator action on /settings/zones), not to quietly accept
     * a gain the check itself has already flagged as physically
     * inconsistent with what the operator told this zone it can reach. */
    /* (C0) Scale-free sanity floor -- runs UNCONDITIONALLY, even when the
     * reachability guard below is disabled (max_temp_c == 0, "no ceiling
     * configured") or degrades to a no-op (no cross-zone coupling ever
     * measured, see below): a real heater cannot have a negative, non-
     * finite, or absurdly large gain. ZONE_MODEL_K_MAX (zones_http.h) is the
     * exact same typo/garbage bound zones_config_set_model() enforces at
     * persist time -- checking it here means a nonsense fit is refused with
     * a reason right where it was computed, instead of silently reaching
     * autotune_engine_accept() and only then failing three steps later at
     * persist time with "feedforward will stay off". Single substitution,
     * well inside the 96-byte abort_reason buffer. */
    if (!isfinite(s_at.model.k_gain_c_per_duty) || s_at.model.k_gain_c_per_duty <= 0.0f ||
        s_at.model.k_gain_c_per_duty > ZONE_MODEL_K_MAX) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "fit failed: fitted gain K = %.4f is not a plausible heater gain",
                 (double)s_at.model.k_gain_c_per_duty);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }

    /* (C) Ceiling reachability -- 2026-08-31 fix. The ORIGINAL version of
     * this check asked "can this ONE zone's own heater, alone, at full
     * duty, reach this zone's configured ceiling" and rejected the fit if
     * not. That question is simply wrong on a COUPLED multi-zone kiln: two
     * consecutive real runs tonight fit K~=39.4 and K~=40.6 against an
     * 80.0C zone limit with step_ambient_c ~=29.8-30.6C (implied ~69-70C
     * alone) and BOTH were correct fits -- the trace tail was fully settled
     * (slope ~0.001 C/s) at ~70C. This zone's own heater genuinely cannot
     * reach 80C by itself; 80C is only reachable with the other zones'
     * measured 5-12 degC/duty of cross-heating added in. The old check
     * rejected both runs as "fit is wrong" even though nothing was wrong.
     *
     * Fix: fold in this zone's MEASURED cross-coupling from every other
     * zone, each assumed driven at full duty too -- the same "can the whole
     * plant configuration reach the ceiling" question, just answered
     * honestly instead of one heater in isolation. zones_config_get_coupling
     * (zones_http.c) returns ROW s_at.zone_index; s_at.zone_index is the
     * AFFECTED zone here (the one whose ceiling is being checked), and
     * out_row[j] is ITS measured response to zone j's heater -- zones_http.h
     * ~line 158's "row i is the affected zone, column j is the stepped
     * zone" convention, the SAME orientation autotune_finalize_fit()'s own cross-gain
     * fit below this block writes. Do NOT transpose: the wire orientation at
     * /api/autotune/matrix is the OPPOSITE of this storage orientation, and
     * that exact swap has already been made once by mistake.
     *
     * ZERO-SEMANTICS DEGRADE PATH: an unmeasured coupling cell defaults to
     * 0.0 (zones_http.h's own "never measured" convention), which this
     * function cannot tell apart from "genuinely zero coupling measured".
     * A kiln that has never run a step test on ANY zone therefore reads an
     * all-zero row -- folding zeros in would silently reproduce the OLD,
     * just-proven-wrong single-zone-only test on exactly the coupled kiln
     * this fix targets (always-reject risk). The opposite extreme -- always
     * skipping the reachability question whenever data is thin -- would
     * let a genuinely undersized/wrong fit through unnoticed (always-pass
     * risk). This function picks neither extreme: when NO coupling has ever
     * been measured for this zone, the reachability question is left
     * unanswered (skipped) rather than answered wrong in either direction,
     * and the fit still has to clear (C0) above (positive/finite/bounded
     * gain) and (B) above (minimum excursion) -- a real sanity floor, just
     * not a reachability claim this function has no data to back. Once ANY
     * neighbor's coupling into this zone has been measured (a later run
     * stepped that neighbor and fit this zone as a peer, see autotune_finalize_fit()'s
     * cross-gain loop below), the full reachability test engages again.
     *
     * ZERO-SEMANTICS TRAP (max_temp_c): max_temp_c == 0 means the ceiling
     * guard is DISABLED for this zone (thermal_guard.c's convention,
     * mirrored everywhere else in this file), NOT "zero degrees is the
     * ceiling" -- this whole block, C0 excepted, only runs when
     * max_temp_c > 0.0f, or every zone that has never had a ceiling
     * configured would have every step test refused.
     *
     * Kept a hard abort rather than an override-able warning when it DOES
     * fire (unlike the settled-flag case, see autotune_engine_accept()'s own
     * comment): an implied ceiling below a CONFIGURED max_temp_c, even with
     * measured coupling folded in, means either the fit is wrong or the
     * zone's own configuration is wrong, and the fix in the latter case is
     * to correct max_temp_c (a deliberate, infrequent operator action on
     * /settings/zones), not to quietly accept a gain this check has already
     * flagged as physically inconsistent with what the operator told this
     * zone (and its neighbors) can reach. */
    if (configured_max_temp_c > 0.0f) {
        float coupling_row[MAX31856_CHANNEL_COUNT] = {0};
        bool have_coupling_row = zones_config_get_coupling(s_at.zone_index, coupling_row);
        uint8_t coupling_thermo_count = zones_config_get_thermo_count();
        float cross_contribution_c = 0.0f;
        bool any_coupling_measured = false;
        if (have_coupling_row) {
            for (uint8_t j = 0; j < coupling_thermo_count && j < MAX31856_CHANNEL_COUNT; j++) {
                if (j == s_at.zone_index) {
                    continue; /* diagonal is always 0, see zones_config_get_coupling()'s own comment */
                }
                if (coupling_row[j] > 0.0f) {
                    any_coupling_measured = true;
                    cross_contribution_c += coupling_row[j]; /* full duty on zone j: u_j = 1.0 */
                }
            }
        }
        if (any_coupling_measured) {
            float implied_max_c = s_at.step_ambient_c + s_at.model.k_gain_c_per_duty + cross_contribution_c;
            if (implied_max_c < configured_max_temp_c) {
                force_relays_off();
                s_at.state = AUTOTUNE_ENGINE_ABORTED;
                /* Kept short and to the point (both numbers, no prose
                 * padding) -- s_at.abort_reason is a fixed 96-byte buffer
                 * shared by every refusal in this function, and the two
                 * floats already eat a good chunk of it; a wordier version
                 * was found to truncate before the second number even
                 * printed. */
                snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                         "fit failed: gain+coupling implies %.1fC max, below zone limit %.1fC -- fit is wrong",
                         (double)implied_max_c, (double)configured_max_temp_c);
                ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
                return;
            }
        }
        /* else: no coupling has ever been measured for this zone -- the
         * reachability question is left unanswered rather than answered
         * wrong (see the block comment above). (C0) and (B) still applied. */
    }

    s_at.proposed_gains = pid_autotune_tune_from_fopdt(&s_at.model, s_at.step_rule, 0.0f);
    s_at.predicted_max_ramp_c_per_hr =
        pid_autotune_estimate_max_ramp_c_per_hr(&s_at.model, 1.0f, s_at.actual_valid ? s_at.actual_c : baseline_c,
                                                baseline_c);
    /* Review finding: the field above is evaluated at t_now_c = end-of-step
     * temperature (the hottest point this run reached), which makes it the
     * SMALLEST achievable rate from that point on -- but every consumer of
     * a stored max_ramp_c_per_hr (profile_executor_run.c, profile_
     * feasibility.c, profiles_edit_http.c) treats it as a temperature-
     * independent hard block, checked from a cold start. Adopting the
     * end-of-step value into that ceiling can refuse a genuinely startable
     * profile. Evaluating at t_now_c == t_ambient_c makes the (T_now -
     * T_ambient) term drop out of pid_autotune_estimate_max_ramp_c_per_hr(),
     * reducing to K*u_max/tau*3600 -- the sustained rate available from a
     * cold start, which is the only value safe to adopt as that ceiling.
     * See autotune_engine_status_t::predicted_max_ramp_ambient_c_per_hr. */
    s_at.predicted_max_ramp_ambient_c_per_hr =
        pid_autotune_estimate_max_ramp_c_per_hr(&s_at.model, 1.0f, baseline_c, baseline_c);
    /* Ambient-evaluated companion (t_now_c == t_ambient_c collapses the
     * estimator to K*u_max/tau*3600, dropping the (T_now - T_ambient) term):
     * the rate the plant can sustain from a cold start, not the smallest
     * rate left at the hottest point this run reached. This is the value
     * autotune_engine_accept() (with opts->adopt_ceiling true) adopts into
     * the zone's ramp ceiling -- see autotune_engine.h's field comment. */

    /* TODO.md 6A.5(b): fill row zone_index of the coupling matrix -- the
     * direct cell (i==i) is this same model, every other configured zone
     * gets its own fit against the identical duty step. A zone whose
     * baseline was never valid (no sensor, or thermo_count doesn't cover
     * it) is left invalid rather than fit against garbage. */
    uint8_t thermo_count = zones_config_get_thermo_count();
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        autotune_coupling_cell_t *cell = &s_at.coupling.cell[s_at.zone_index][j];
        if (j == s_at.zone_index) {
            cell->model = s_at.model;
            cell->valid = true;
            continue;
        }
        if (j >= thermo_count || !s_at.zone_baseline_valid[j]) {
            cell->valid = false;
            continue;
        }
        autotune_sample_t *peer = autotune_unpack_zone_trace(j, s_at.trace_count);
        if (!peer) {
            /* Only the cross-gain cell is lost; the direct fit above already
             * succeeded and the tuning it produced stands. */
            cell->valid = false;
            ESP_LOGW(AT_TAG, "autotune zone %u: out of memory fitting cross-gain against zone %u",
                     s_at.zone_index, j);
            continue;
        }
        fopdt_model_t m = pid_autotune_fit_fopdt(peer, s_at.trace_count, s_at.zone_baseline_c[j], s_at.step_duty);
        free(peer);
        cell->model = m;
        cell->valid = m.valid;
    }

    /* Persist every valid cross-gain cell fitted just above into
     * zones_http.c's NVS-backed coupling row -- until this pass, the whole
     * matrix lived in s_at.coupling (RAM only, see this struct's own field
     * comment); nothing ever called zones_config_set_coupling*(), so a step
     * test's measured cross-coupling died at the next reboot even though
     * zone i's own direct model (model_k_dc etc, right above) was already
     * being saved. A step test costs the operator hours of real kiln heat --
     * re-measuring it every boot is not an option, same reasoning
     * model_k_dc's own persistence exists for.
     *
     * What gets stored, and its units: cell[zone_index][j].model is the
     * FOPDT fit of ZONE j's own trace against the duty step commanded at
     * zone_index's heater -- i.e. "how much does zone_index's heater move
     * zone j". That is exactly zone j's row, column zone_index in
     * zones_http.c's coupling_coeff[] (row i = zone i's measured response to
     * a unit step at neighbor j's heater -- see zone_cfg_t's own doc
     * comment). So this writes zones_config_set_coupling_cell(j,
     * zone_index, ...): the AFFECTED zone owns the row, the zone under test
     * is the column. The stored number is model.k_gain_c_per_duty AS
     * MEASURED -- a raw degC-per-unit-duty steady-state gain, the same unit
     * convention model_k_dc already uses, NOT a ratio to any self-gain --
     * so the feedforward term (a later pass) can combine coupling_coeff[]
     * cells with model_k_dc directly with no extra scaling step.
     *
     * Guarded twice against a bad or aborted fit:
     *   (1) this whole function already returned early above if s_at.model
     *       (the DIRECT zone_index<-zone_index fit) was invalid -- an
     *       aborted run never reaches here at all, so it can never persist
     *       anything, direct or cross.
     *   (2) each cross cell is persisted only if cell->valid AND its gain is
     *       finite and within ZONE_COUPLING_COEFF_MAX's bound (the same
     *       range zones_config_set_coupling_cell() itself enforces) -- a
     *       cell whose peer fit failed, or whose gain landed outside the
     *       storage layer's accepted range (e.g. a spurious negative
     *       reading -- see ZONE_COUPLING_COEFF_MAX's own "why non-negative"
     *       comment in zones_http.h), is skipped rather than clobbering a
     *       previously-stored good value for that same neighbor with 0 or a
     *       rejected write.
     *
     * 2026-08-31 PANIC FIX -- confirmed on hardware, coredump decoded:
     * task_entry() (this whole function's caller) runs on a PSRAM-stacked
     * task (see autotune_engine_start()'s xTaskCreatePinnedToCoreWithCaps()
     * call and its own comment), and zones_config_set_coupling_cell() ends in
     * zones_http.c's nvs_save(), which disables the flash cache. A task whose
     * stack lives in PSRAM cannot survive that -- ESP-IDF's
     * esp_task_stack_is_sane_cache_disabled() asserts and reboots the whole
     * board the instant a real fit reaches here (it did, first time out:
     * autotune_finalize_fit() -> zones_config_set_coupling_cell() -> nvs_save() ->
     * cache disable -> assert failed: spi_flash_disable_interrupts_caches_
     * and_other_cpu, cache_utils.c:126). Exactly the hazard uart_bridge_ext.c:
     * 104-127's HAZARD block documents for CONTROL/PROFILES/AUTOTUNE (the
     * UART bridge tasks) and safety_cfg_store.c's nvs_save_store() documents
     * for safety_poll_task -- this call site was simply never audited for it
     * because the persist call itself is new this session (previously
     * autotune_finalize_fit() only ever wrote s_at.coupling, RAM-only).
     *
     * Fixed the same way both of those precedents were: every cell to write
     * is gathered into coupling_persist_job_t below (RAM only, no flash
     * touched yet), and the actual zones_config_set_coupling_cell() calls run
     * as ONE job on bx_flash_worker (uart_bridge_ext_run_on_flash_worker()),
     * whose stack is ordinary internal SRAM. This function still decides
     * WHICH cells are eligible (guards (1) and (2) above); the worker only
     * ever writes what this function already validated. */
    /* 2026-09-02 review fix (round 2, item 4; extended round-3 follow-up):
     * this whole block used to run unconditionally on every DONE run,
     * including one that reached DONE via the max-duration backstop with
     * s_at.model.settled == false -- the SAME truncated, low-K-biased peer
     * traces the (B)/(C) refusals above exist to catch on the DIRECT fit,
     * persisted to flash for every OTHER zone's coupling row before the
     * operator ever sees an Accept button, let alone an ack_unsettled
     * checkbox. autotune_engine_accept()'s gate only covers the direct
     * model + gains (zones_config_set_model()/set_pid()); this persist
     * call was never behind it at all. Gated here on the SAME THREE
     * conditions accept() now uses for the direct fit (settled AND
     * extrapolation_converged AND tau_consistent_with_gain -- see
     * autotune_engine_accept()'s own comment), not settled alone.
     *
     * Argued explicitly, per review request, why extrapolation_converged/
     * tau_consistent_with_gain should ALSO gate coupling persist, not just
     * settled: those two flags are properties of THIS SAME (direct,
     * stepped-zone) fit's asymptote correction, computed from the SAME
     * duty step and the SAME slope_end/tau mechanics that every peer
     * zone's own pid_autotune_fit_fopdt() call below reuses on ITS trace.
     * An unconverged or tau-inconsistent direct fit is strong evidence the
     * whole run's trace shape (truncation, noise, dead-time detection) was
     * marginal, not something specific to the direct zone alone -- exactly
     * the same reasoning settled's own gate already rests on (a run-level
     * property of STEPPING, not a per-zone one). Persisting peer coupling
     * cells from a run whose own direct fit could not be trusted enough to
     * auto-accept would reintroduce the identical bypass round-2's review
     * already caught once for settled alone. A settled-AND-converged-AND-
     * consistent run's cross-gains are unaffected -- this is strictly a
     * new refusal on the low-confidence path, not a behavior change on the
     * honest one. */
    if (s_at.model.settled && s_at.model.extrapolation_converged && s_at.model.tau_consistent_with_gain) {
        coupling_persist_job_t job = {0};
        job.stepped_zone = s_at.zone_index;
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (j == s_at.zone_index) {
                continue;
            }
            const autotune_coupling_cell_t *cell = &s_at.coupling.cell[s_at.zone_index][j];
            if (!cell->valid) {
                continue;
            }
            float gain = cell->model.k_gain_c_per_duty;
            if (!isfinite(gain) || gain < 0.0f || gain > ZONE_COUPLING_COEFF_MAX) {
                ESP_LOGW(AT_TAG,
                         "autotune zone %u: cross-gain against zone %u (%.4f degC/duty) out of storage range, "
                         "not persisted",
                         s_at.zone_index, j, (double)gain);
                continue;
            }
            /* ZONES_CFG_VERSION 11->12: the same peer fit's tau/dead-time,
             * gated the same way -- finite and within the storage layer's
             * bound (ZONE_MODEL_TIME_MAX_S, the same ceiling model_tau_s/
             * model_dead_time_s use) -- so a spurious fit cannot land a
             * garbage gain paired with a garbage time constant, or vice
             * versa. zones_config_set_coupling_cell() itself re-checks this
             * (all-or-nothing per cell); this mirrors that check up front so
             * the reason for skipping a cell is logged with the same detail
             * the gain check above already gets. */
            float tau_s = cell->model.tau_s;
            float dead_time_s = cell->model.dead_time_s;
            if (!isfinite(tau_s) || tau_s < 0.0f || tau_s > ZONE_MODEL_TIME_MAX_S || !isfinite(dead_time_s) ||
                dead_time_s < 0.0f || dead_time_s > ZONE_MODEL_TIME_MAX_S) {
                ESP_LOGW(AT_TAG,
                         "autotune zone %u: cross-gain against zone %u fitted tau=%.1fs L=%.1fs out of storage "
                         "range, not persisted",
                         s_at.zone_index, j, (double)tau_s, (double)dead_time_s);
                continue;
            }
            job.affected_zone[job.count] = j;
            job.coeff[job.count] = gain;
            job.tau_s[job.count] = tau_s;
            job.dead_time_s[job.count] = dead_time_s;
            job.count++;
        }
        if (job.count > 0) {
            /* Flash-worker lock-inversion audit 2026-10-09 F2: this runs under
             * s_at.lock, so the job is NOT dispatched here. task_entry()
             * takes it after the tick and dispatches it once s_at.lock is
             * given -- see autotune_dispatch_coupling_persist(). The job is
             * a by-value snapshot, so nothing the tick changes afterwards
             * can alter what gets written. */
            s_at.pending_coupling = job;
            s_at.pending_coupling_valid = true;
        }
    } else {
        /* Deliberately not wired to autotune_engine_accept(): a later
         * ack_unsettled=true accept persists the DIRECT model/gains for
         * THIS zone but does not retroactively persist the cross-gain
         * cells measured here -- those stay RAM-only (s_at.coupling, still
         * visible on /settings/zones this session) and are lost on reboot,
         * same as an unsettled run always eventually was for a zone whose
         * accept was refused outright. Re-running the step test to genuine
         * settlement is the only way to persist coupling; simply the
         * safest option given this is peer-zone data the operator has no
         * per-cell accept UI for at all. */
        ESP_LOGW(AT_TAG, "autotune zone %u: cross-gain coupling cells NOT persisted -- direct fit not fully "
                      "trustworthy (settled=%d converged=%d tau_consistent=%d); re-run the test to "
                      "persist them",
                 s_at.zone_index, (int)s_at.model.settled, (int)s_at.model.extrapolation_converged,
                 (int)s_at.model.tau_consistent_with_gain);
    }

    force_relays_off();
    s_at.state = AUTOTUNE_ENGINE_DONE;
    ESP_LOGI(AT_TAG, "autotune zone %u done: K=%.2f tau=%.1fs L=%.1fs -> Kp=%.5f Ki=%.5f Kd=%.5f", s_at.zone_index,
             (double)s_at.model.k_gain_c_per_duty, (double)s_at.model.tau_s, (double)s_at.model.dead_time_s,
             (double)s_at.proposed_gains.kp, (double)s_at.proposed_gains.ki, (double)s_at.proposed_gains.kd);
}

/* Target-temperature mode only: called instead of autotune_finalize_fit() when
 * PHASE 1 (the probe) ends -- either AUTOTUNE_ENGINE_PROBE_DURATION_S
 * elapsed, or (a fast/strong zone) the ordinary settle detector fired early.
 *
 * Fits a rough FOPDT gain from the probe's own trace by calling
 * pid_autotune_fit_fopdt() -- the SAME fit autotune_finalize_fit() itself uses, not a
 * second estimator, just applied to the probe's (small, low-duty) step
 * instead of the real one. Uses that rough K to pick the duty PHASE 2 needs
 * to land its asymptote at s_at.target_c, refuses clearly if the target is
 * out of reach at full duty, and otherwise rewinds the state machine to
 * SETTLING with the new duty so the UNMODIFIED SETTLING->STEPPING->
 * autotune_finalize_fit() path runs the real identification step -- every guard
 * autotune_finalize_fit() applies (ceiling headroom, physical plausibility, minimum
 * excursion) therefore applies to the identification step exactly as it
 * does to a plain duty-based run. */
void handle_probe_done_locked(void)
{
    float baseline_c = s_at.zone_baseline_c[s_at.zone_index];

    autotune_sample_t *scratch = autotune_unpack_zone_trace(s_at.zone_index, s_at.trace_count);
    if (!scratch) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "out of memory unpacking a %u-sample probe trace", (unsigned)s_at.trace_count);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    fopdt_model_t probe = pid_autotune_fit_fopdt(scratch, s_at.trace_count, baseline_c, AUTOTUNE_ENGINE_PROBE_DUTY);
    free(scratch);
    if (!probe.valid) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "probe fit failed: %s", probe.invalid_reason);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    if (!(probe.k_gain_c_per_duty > 0.0f)) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "probe gain %.3f is non-positive -- cannot derive a duty for target %.1fC",
                 (double)probe.k_gain_c_per_duty, (double)s_at.target_c);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    s_at.probe_k_rough = probe.k_gain_c_per_duty;

    /* duty = (target - baseline) / K_rough -- the duty that, at this rough
     * gain, asymptotes the step response at target_c. Refused, not clamped,
     * when it falls outside (0, 1]: a target at or below the probe baseline
     * needs a negative (impossible) duty, and a target above what full duty
     * can reach needs a duty > 1.0 -- both are told apart below so the
     * refusal names the actual problem rather than one generic message. */
    float reach = (s_at.target_c - baseline_c) / probe.k_gain_c_per_duty;
    if (!(reach > 0.0f)) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "target %.1fC is not above the %.1fC probe baseline", (double)s_at.target_c, (double)baseline_c);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }
    if (reach > 1.0f) {
        /* Highest temperature this rough gain says the zone CAN reach, at
         * duty 1.0 -- the number the caller asked this refusal to name.
         * probe_k_rough itself is separately visible through
         * autotune_engine_get_status() (out->probe_k_rough), so it does not
         * also have to be crammed into this 96-byte, two-float-substitution
         * buffer alongside the target and the reachable max. */
        float reachable_max_c = baseline_c + probe.k_gain_c_per_duty;
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "target %.1fC unreachable at full duty (est max ~%.1fC)", (double)s_at.target_c,
                 (double)reachable_max_c);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }

    /* Review round-3 finding 3: reach > 0 alone does not bound how SMALL
     * identify_duty can be -- a target only a hair above baseline (a tight
     * re-tune, or a probe-fit K_rough that overshoots the true gain) yields
     * e.g. reach ~= 0.002, below AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST
     * (0.01). Below that floor guard 1/2's progress window
     * (thermal_guard.c's commanded_duty >= progress_duty_min gate) never
     * arms AT ALL for the entire identify phase -- not merely a weaker bar
     * (blocker 4's duty scaling), an absent one -- and even armed, the
     * duty-scaled bar (blocker 4) would itself be within rounding of zero.
     * autotune_finalize_fit()'s own minimum-excursion check (autotune_min_rise_c())
     * still refuses a fit this small eventually, so this was never a heat-
     * safety hole -- but it silently ran the identify phase's whole
     * multi-hour budget with the progress guards disarmed. Refused here,
     * before PHASE 2 ever starts, rather than merely clamped up to the
     * floor (which would identify at a duty the operator's target did not
     * actually ask for). */
    if (reach < AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST) {
        force_relays_off();
        s_at.state = AUTOTUNE_ENGINE_ABORTED;
        /* Two float substitutions only (reach, target_c) -- the floor
         * itself is a compile-time constant, written as a literal below
         * rather than a third %f substitution, matching this file's own
         * "at most two float substitutions per 96-byte abort_reason"
         * -Werror=format-truncation discipline (see autotune_finalize_fit()'s (B)
         * refusal comment for the original incident this convention
         * traces back to). */
        snprintf(s_at.abort_reason, sizeof(s_at.abort_reason),
                 "identify duty %.4f for target %.1fC is below the 0.01 floor guard 1/2 need to arm",
                 (double)reach, (double)s_at.target_c);
        ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
        return;
    }

    float identify_duty = reach;
    if (identify_duty > 1.0f) identify_duty = 1.0f; /* defensive -- reach already <= 1.0f on this path */

    ESP_LOGI(AT_TAG, "autotune zone %u: probe done, K_rough=%.3f baseline=%.1fC -- identifying at duty %.3f for "
                  "target %.1fC",
             s_at.zone_index, (double)probe.k_gain_c_per_duty, (double)baseline_c, (double)identify_duty,
             (double)s_at.target_c);

    s_at.probe_phase = false;
    s_at.step_duty = identify_duty;
    /* Rewinds to SETTLING exactly as autotune_begin_run_locked() left it before the
     * probe's own SETTLING->STEPPING transition -- that transition code is
     * unmodified and will capture a fresh baseline_c (wherever the zone
     * actually is after the probe and this re-settle, not necessarily back
     * at the original cold baseline) before stepping to identify_duty. */
    s_at.state = AUTOTUNE_ENGINE_SETTLING;
    s_at.phase_start_tick = xTaskGetTickCount();
    /* Re-arm the readiness-check capture for this re-settle -- see
     * readiness_start_captured's own comment. Without this the probe
     * phase's own start-of-SETTLING reading would be compared against
     * temperatures reached AFTER the probe's own heat, which is not what
     * the readiness check is for. */
    s_at.readiness_start_captured = false;
}

/* Writes one row of the packed trace, for both methods.
 *
 * Only the tested zone's row is ever read back for a relay run: the coupling
 * matrix (TODO.md 6A.5(b)) is fitted with pid_autotune_fit_fopdt() against a
 * known duty *step*, and a relay run never applies one, so a relay run
 * deliberately leaves the matrix untouched rather than filling it with cells
 * fitted against an oscillation. The peer rows are still recorded because
 * MAX31856_read_all() already read them and a future cross-gain-from-relay
 * method would want them; they cost nothing extra here.
 *
 * Must be called with s_at.lock held. */
void record_trace_sample(const float *raw_by_zone, const bool *ok_by_zone)
{
    if (s_at.trace_count >= AUTOTUNE_ENGINE_MAX_SAMPLES) {
        return;
    }
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        float c = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : s_at.zone_last_valid_c[z];
        if (ok_by_zone[z]) s_at.zone_last_valid_c[z] = c;
        /* Packed: t is implicit in the index (see autotune_engine.h),
         * temperature is 0.1degC. */
        int16_t dc;
        if (isnan(c)) {
            dc = AUTOTUNE_TRACE_TEMP_INVALID;
        } else {
            float scaled = c * 10.0f;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32767.0f) scaled = -32767.0f;
            dc = (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        }
        s_at.zone_trace[z][s_at.trace_count] = dc;
    }
    s_at.trace_count++;
}

/* (A) Peak-slope tracking + the honest settle check, run once per newly-
 * recorded STEPPING sample (s_at.trace_count-1 is that sample's index,
 * s_at.elapsed_s its stepping-relative time). Factored out of
 * autotune_engine_tick_locked() into its own seam for exactly the reason
 * that function itself was factored out of task_entry() (see its own
 * comment): test_autotune_engine_prestart.c's settle-detector tests call
 * this directly against a synthetic trace written straight into
 * s_at.zone_trace (the same technique write_synthetic_fopdt_trace_for_zone()
 * already uses for autotune_finalize_fit()'s own tests), one sample at a time with
 * s_at.elapsed_s advanced by hand -- this host build's frozen
 * xTaskGetTickCount() stub makes driving 1000+ real ticks to reach a
 * multi-tau settle window impractical, and duplicating this logic in the
 * test file instead of calling the real one would let the two drift apart
 * silently. See SETTLE_RELATIVE_SLOPE_FRAC's own comment above for what the
 * single criterion below means and why (and why there is only one, not two,
 * as of the 2026-09-01 review fix). Must be called with s_at.lock held, only
 * while STEPPING, only after this sample's record_trace_sample() (or its
 * skip) has already happened. Returns true exactly when the trace should be
 * finalized as genuinely settled -- the caller (either the tick loop or a
 * test) is responsible for actually calling autotune_finalize_fit(). */
bool step_settle_check_locked(void)
{
    /* Peak-slope tracking -- item (4)/(4b) fix: a filtered, onset-anchored
     * estimate instead of a raw adjacent-sample difference over the whole
     * run. The windowed (first-to-last-of-PEAK_SLOPE_WINDOW_SAMPLES) slope
     * is computed every sample once enough exist; response onset (this
     * run's step_onset_seen) latches the FIRST time that windowed slope
     * clears RESPONSE_ONSET_SLOPE_C_PER_S, wherever in the run that happens
     * -- no fixed sample-count cutoff, so an arbitrarily long dead time (see
     * item (4b)'s own comment for the >300s defect this replaces) is
     * handled the same as a short one. The peak is only tracked for
     * PEAK_SLOPE_SEARCH_SAMPLES samples STARTING AT onset, not from run
     * start, so a late noise spike deep into the run (long after onset AND
     * its search window have both passed) still cannot inflate the peak. */
    if (s_at.trace_count >= PEAK_SLOPE_WINDOW_SAMPLES) {
        int16_t dc_prev = s_at.zone_trace[s_at.zone_index][s_at.trace_count - PEAK_SLOPE_WINDOW_SAMPLES];
        int16_t dc_cur = s_at.zone_trace[s_at.zone_index][s_at.trace_count - 1];
        if (dc_prev != AUTOTUNE_TRACE_TEMP_INVALID && dc_cur != AUTOTUNE_TRACE_TEMP_INVALID) {
            float v_prev = (float)dc_prev / 10.0f;
            float v_cur = (float)dc_cur / 10.0f;
            float window_s = (float)((PEAK_SLOPE_WINDOW_SAMPLES - 1) * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
            float slope = (v_cur - v_prev) / window_s;
            float aslope = fabsf(slope);

            /* Review blocker 2: this used to test aslope (fabsf(slope)) --
             * direction-blind. In target mode the identify phase re-baselines
             * after only a 180s re-settle (AUTOTUNE_ENGINE_SETTLE_S) on a
             * zone still cooling from the probe -- a zone 30C above ambient
             * with tau=285s cools at ~0.105 C/s, 11x RESPONSE_ONSET_SLOPE_
             * C_PER_S (0.009), so the OLD magnitude-only test latched onset
             * on residual COOLING before the element had done anything,
             * defeating this defense entirely and leaving the 3.0C absolute
             * rise (AUTOTUNE_ELEMENT_ALIVE_RISE_C) as the sole gate -- the
             * exact gate blocker 1 (cross-zone coupling) can also defeat.
             * Two independent defenses were meant to compose; a direction-
             * blind onset test silently removed one of them in exactly the
             * scenario (target mode) that most needs both. Fixed by testing
             * the SIGNED slope, not its magnitude: a real step response
             * always rises (this file's convention is a positive duty step
             * from a lower baseline -- see autotune_engine_run()'s own doc
             * comment), so onset now means "this zone is genuinely heating",
             * never "this zone's temperature moved, in either direction".
             * Every EXISTING settle-detector test drives a rising synthetic
             * trace (K positive), so this is behaviorally transparent for
             * all of them -- it only changes behavior for a falling
             * response, which was never a genuine step-response case this
             * detector was built to recognize. */
            if (!s_at.step_onset_seen && slope > RESPONSE_ONSET_SLOPE_C_PER_S) {
                s_at.step_onset_seen = true;
                s_at.step_onset_trace_count = s_at.trace_count;
            }
            if (s_at.step_onset_seen &&
                s_at.trace_count <= (uint32_t)s_at.step_onset_trace_count + PEAK_SLOPE_SEARCH_SAMPLES &&
                aslope > s_at.step_peak_slope_c_per_s) {
                s_at.step_peak_slope_c_per_s = aslope;
                s_at.step_peak_slope_at_s = s_at.elapsed_s;
            }
        }
    }

    /* Item (9), a latent defect found in review: once trace_count reaches
     * AUTOTUNE_ENGINE_MAX_SAMPLES, record_trace_sample() stops appending
     * (buffer full) but this function would otherwise keep re-reading the
     * SAME frozen trailing SETTLE_CHECK_SAMPLES window every subsequent
     * tick -- recent_slope goes to exactly 0 (no new data, not genuine
     * settling) and the detector would fire true on a trace that may still
     * have been climbing the instant it got truncated. Today the 4h
     * max-duration backstop happens to win the race first for every current
     * constant combination, but that is a coincidence of timing, not a
     * guarantee -- explicit here so a future change to AUTOTUNE_ENGINE_
     * SAMPLE_PERIOD_S or AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S cannot
     * silently reopen this. A full trace is handed to the max-duration path
     * instead (return false -> autotune_finalize_fit() is reached via the elapsed_s
     * check in the caller, which correctly marks step_settled=false). */
    if (s_at.trace_count >= AUTOTUNE_ENGINE_MAX_SAMPLES) {
        return false;
    }
    if (s_at.trace_count < MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK || s_at.trace_count < SETTLE_CHECK_SAMPLES ||
        s_at.step_peak_slope_c_per_s <= SETTLE_ABS_SLOPE_FLOOR_C_PER_S) {
        return false;
    }
    int16_t dc_first = s_at.zone_trace[s_at.zone_index][s_at.trace_count - SETTLE_CHECK_SAMPLES];
    int16_t dc_last = s_at.zone_trace[s_at.zone_index][s_at.trace_count - 1];
    if (dc_first == AUTOTUNE_TRACE_TEMP_INVALID || dc_last == AUTOTUNE_TRACE_TEMP_INVALID) {
        return false;
    }
    float v_first = (float)dc_first / 10.0f;
    float v_last = (float)dc_last / 10.0f;
    float window_s = (float)((SETTLE_CHECK_SAMPLES - 1) * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
    float recent_slope = fabsf((v_last - v_first) / window_s);

    /* The single settle criterion: recent slope has decayed to a small
     * fraction of this run's (early-region, filtered) peak, or is itself
     * below the absolute noise floor -- a plant that has genuinely stopped
     * moving. See this function's header comment for why the "elapsed time
     * >= N*tau" criterion this used to also require was removed rather than
     * kept as decoration: it was algebraically implied by this one. */
    return (recent_slope <= SETTLE_RELATIVE_SLOPE_FRAC * s_at.step_peak_slope_c_per_s) ||
           (recent_slope <= SETTLE_ABS_SLOPE_FLOOR_C_PER_S);
}

/* Target-mode PHASE 1 (probe) self-termination -- see AUTOTUNE_PROBE_GAIN_
 * STABLE_FRAC's own comment above for the full reasoning. Must be called
 * with s_at.lock held, only while probing (s_at.target_mode &&
 * s_at.probe_phase), only after this sample's record_trace_sample() has
 * already happened this tick -- same calling convention as
 * step_settle_check_locked(), and called from the exact same call site,
 * right alongside it.
 *
 * Re-fits pid_autotune_fit_fopdt() -- the SAME function handle_probe_done_
 * locked() itself uses for the final probe fit, not a second estimator --
 * against the trace recorded so far. A fit that is not yet valid (too few
 * points, no onset yet) or whose gain is non-positive is treated as "not
 * converged" and also resets the dwell counter, exactly like any other
 * disagreement between consecutive estimates: a momentarily invalid fit is
 * not evidence of stability. */
bool probe_gain_converged_locked(void)
{
    if (s_at.trace_count < MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK) {
        return false; /* same floor step_settle_check_locked() uses -- too early to trust any fit */
    }
    autotune_sample_t *scratch = autotune_unpack_zone_trace(s_at.zone_index, s_at.trace_count);
    if (!scratch) {
        return false; /* OOM -- treat as not-yet-converged, the whole-run/phase budgets remain the backstop */
    }
    float baseline_c = s_at.zone_baseline_c[s_at.zone_index];
    fopdt_model_t fit = pid_autotune_fit_fopdt(scratch, s_at.trace_count, baseline_c, AUTOTUNE_ENGINE_PROBE_DUTY);
    free(scratch);
    if (!fit.valid || !(fit.k_gain_c_per_duty > 0.0f)) {
        s_at.probe_stable_checks = 0u;
        return false;
    }
    float k_new = fit.k_gain_c_per_duty;
    if (s_at.probe_last_k_c_per_duty > 0.0f) {
        float rel_change = fabsf(k_new - s_at.probe_last_k_c_per_duty) / s_at.probe_last_k_c_per_duty;
        if (rel_change <= AUTOTUNE_PROBE_GAIN_STABLE_FRAC) {
            if (s_at.probe_stable_checks < UINT16_MAX) {
                s_at.probe_stable_checks++;
            }
        } else {
            s_at.probe_stable_checks = 0u;
        }
    } else {
        s_at.probe_stable_checks = 0u; /* first estimate this phase -- nothing to compare against yet */
    }
    s_at.probe_last_k_c_per_duty = k_new;
    return s_at.probe_stable_checks >= AUTOTUNE_PROBE_GAIN_STABLE_DWELL;
}

/* Phase 7c pre-start thermal readiness check (2026-08-31 hardware finding;
 * REVISED 2026-08-31 after checking the first version against real
 * accept/refuse data from tonight's rig -- it failed the accept side, see
 * "THE REFERENCE PROBLEM" below):
 *
 * autotune_finalize_fit()'s gain comes out of raw_rise = final_c - baseline_c, and
 * baseline_c is captured once, at the SETTLING->STEPPING transition below.
 * If a zone still carries residual heat at that instant -- either the zone
 * UNDER TEST (its own baseline is high, so raw_rise is too small) or a
 * NEIGHBOUR zone that is still cooling (its coupled heat bleeds into the
 * tested zone and decays over the run, subtracting from the apparent rise
 * the same direction) -- the fitted gain comes out low by roughly the
 * baseline error, and a low gain over-drives the feedforward term
 * downstream. Measured on this board: a zone started "slightly hot" fit
 * K=36.4 (rejected, and wrong); the same zone from a genuinely rested
 * 31.36C fit K=39.25 with every confidence flag true, matching an
 * independent least-squares fit of the raw trace (~39.1-40.0).
 *
 * THE REFERENCE PROBLEM: the obvious reference for "how hot is this zone
 * right now" is the cold-junction reading (step_ambient_c) -- it is on the
 * board, always available, and autotune_finalize_fit()'s own physical-plausibility
 * check already uses it. But the CJ measures the BOARD, not the chamber,
 * and the two are not colocated: on this rig a genuinely, fully rested
 * zone reads 1-3C above ITS OWN CJ as a FIXED offset from sensor
 * placement, not leftover warmth. Two real runs proved this the hard way:
 * zone 0's only fully-valid tune ever produced (baseline 31.36C, CJ
 * 29.75C, all three confidence flags true, corroborated by an independent
 * least-squares fit) sits 1.61C above its own CJ; zone 1's run after the
 * operator deliberately cooled the kiln for 25 minutes sits 1.97C above
 * ITS CJ. A margin-above-CJ rule tight enough to catch a genuinely hot
 * zone (tens of C) is nowhere near loose enough to pass either of these --
 * on THIS board it would refuse every tune, forever, which is strictly
 * worse than the bug it exists to prevent. The absolute-ambient version of
 * this rule has been REMOVED for exactly this reason; ambient/CJ is no
 * longer read by this check at all.
 *
 * THE FIX -- two checks, neither referencing ambient:
 *
 * (1) STABILITY, PRIMARY. A zone recently driven is either still cooling
 *     (a clear negative slope) or, briefly, still rising; a genuinely
 *     rested zone is flat REGARDLESS OF ITS ABSOLUTE LEVEL, because the
 *     fixed sensor-placement offset above is CONSTANT and cancels out of
 *     a slope entirely. Reuses SETTLE_ABS_SLOPE_FLOOR_C_PER_S -- the SAME
 *     absolute noise floor the settle detector already uses to call a
 *     STEPPING trace "flat" -- so this is the existing threshold applied
 *     one phase earlier, not a second invented one. Slope is estimated
 *     from the FIRST valid SETTLING-phase reading to the transition
 *     reading (readiness_start_c/readiness_start_captured). Most portable
 *     half of the check: no sensor geometry, no configured ceiling, no CJ
 *     dependency at all.
 *
 * (2) CROSS-ZONE SPREAD, SECONDARY. Slope alone misses a zone that has
 *     already PLATEAUED hot (no longer cooling, but sitting well above
 *     where it belongs) -- unlikely mid-SETTLING but not impossible if the
 *     operator starts back-to-back tunes without a real rest. Every zone
 *     carries its OWN fixed placement offset, but at genuine rest every
 *     zone sits near ITS OWN steady value -- comparing zones AGAINST EACH
 *     OTHER cancels the per-zone offset the same way (1) cancels it over
 *     time. Tonight's own data makes the two populations unmistakable:
 *     rested, the three zones read 32.2 / 30.9 / 30.0 (2.2C spread);
 *     after zone 0's run they read 67.8 / 41.9 / 35.8 (32C spread against
 *     the coolest zone). For each zone z with a valid reading, compare it
 *     against min_baseline_c (the coolest currently-valid zone THIS tick)
 *     in place of ambient:
 *         spread_z = baseline_z - min_baseline_c
 *     refused if spread_z exceeds min_rise_z = autotune_min_rise_c
 *     (baseline_z, max_temp_z) -- the SAME "minimum trustworthy rise"
 *     autotune_finalize_fit() itself refuses a fit below. No fraction is taken of
 *     it (unlike the removed absolute-ambient version): this is a coarse
 *     sanity divider, not a tight gain-error bound, so the full min_rise
 *     is the threshold. Reading: if a zone's excess over the coolest zone
 *     is already as large as the rise a genuine step test itself needs to
 *     trust a fit, that zone is carrying something on the order of an
 *     actual step response, not rest-state scatter -- 2.2C measured on
 *     this rig sits far under min_rise (7+ C at these baselines/ceilings);
 *     32C sits far over it (well under 2C at that zone's much smaller
 *     headroom). No ambient reference, no CJ, no constant tuned to this
 *     kiln -- only autotune_min_rise_c(), already used by autotune_finalize_fit()
 *     and unchanged here.
 *
 * PORTABILITY: neither check assumes anything about where the CJ sits
 * relative to the chamber, so a board with a DIFFERENT (or zero) CJ-to-
 * chamber offset is unaffected either way -- the removed version's
 * failure mode (a board-specific offset making the check permanently
 * strict or permanently loose) cannot recur, because the offset is never
 * read. A kiln with only ONE usable zone loses the cross-zone check
 * (min_baseline_c degenerates to that zone's own baseline, spread_z == 0,
 * always passes) but keeps the stability check, which needs no peers --
 * why (1) is primary and (2) a cross-check, not the reverse: single-zone
 * hardware must still be protected.
 *
 * DEGRADE PATH (chosen over an explicit override parameter, unchanged
 * reasoning from the first version): autotune_engine_run()/_run_to_
 * target() are called only from dashboard_http.c and uart_bridge_ext.c;
 * this pass does not own dashboard_http.c and a silent "always allow"
 * flag would defeat the check's purpose anyway. Instead:
 *   - a zone with no valid reading THIS tick (ok_by_zone[z] false) is
 *     skipped entirely -- cannot judge it, so it cannot block the run,
 *     and it is excluded from the min_baseline_c computation so one dead
 *     sensor cannot drag every OTHER zone's spread check tight or loose;
 *   - a zone whose start-of-SETTLING reading was never captured skips
 *     only the STABILITY half of its check, not the spread half;
 *   - if every zone ends up skipped this way, ready=true (nothing to
 *     refuse) -- so a fully sensor-degraded board can still start,
 *     matching every other guard's fail-open-on-missing-data precedent in
 *     this file (e.g. the no-ceiling branch just above autotune_finalize_fit()).
 * A genuinely hot or genuinely drifting, genuinely sensed zone is never
 * overridable short of the operator waiting for it to settle -- exactly
 * the "the ceiling had never heard of..." class of precedent this repo
 * does not walk back silently.
 *
 * Returns true if ready. On false, `reason` (>= 96 bytes) is filled with
 * a message naming the first offending zone -- kept to ONE zone by design,
 * matching every other refusal in this file's two-float-substitution /
 * -Wformat-truncation discipline (see the identify-duty refusal's own
 * comment); s_at.abort_reason already reports which zone the RUN itself is
 * on, so this only needs to add which OTHER zone is the problem. */
bool check_thermal_readiness_locked(const float *raw_by_zone, const bool *ok_by_zone, char *reason,
                                           size_t reason_cap)
{
    /* Pass 1: coolest currently-valid zone this tick -- the reference (2)
     * compares every zone against, in place of ambient. */
    float min_baseline_c = 0.0f;
    bool  have_min_baseline = false;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        if (!ok_by_zone[z]) continue;
        float c = zones_config_apply_cal(z, raw_by_zone[z]);
        if (!have_min_baseline || c < min_baseline_c) {
            min_baseline_c = c;
            have_min_baseline = true;
        }
    }

    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        if (!ok_by_zone[z]) {
            continue; /* cannot judge -- degrade, do not block */
        }
        float baseline_z_c = zones_config_apply_cal(z, raw_by_zone[z]);

        /* (1) STABILITY -- primary, no sensor-geometry dependency. */
        if (s_at.readiness_start_valid[z]) {
            float slope_z = (baseline_z_c - s_at.readiness_start_c[z]) / (float)AUTOTUNE_ENGINE_SETTLE_S;
            if (fabsf(slope_z) > SETTLE_ABS_SLOPE_FLOOR_C_PER_S) {
                if (reason) {
                    snprintf(reason, reason_cap,
                             "zone %u is still drifting %.4fC/s (above %.4fC/s) -- not settled yet",
                             (unsigned)z, (double)slope_z, (double)SETTLE_ABS_SLOPE_FLOOR_C_PER_S);
                }
                return false;
            }
        }

        /* (2) CROSS-ZONE SPREAD -- secondary, catches a zone that has
         * already plateaued hot instead of still visibly cooling. */
        if (have_min_baseline) {
            float max_temp_z = 0.0f, min_temp_z = -20.0f;
            zones_config_get_temp_limits(z, &max_temp_z, &min_temp_z);
            /* No run-specific probe estimate exists for zone z here -- this
             * check runs pre-STEPPING, judging OTHER zones' rest state, not
             * the zone under test's own measured plant. 0.0f falls back to
             * the original unscaled constants (see autotune_min_rise_c()'s
             * own comment). */
            float min_rise_z = autotune_min_rise_c(baseline_z_c, max_temp_z, 0.0f);
            float spread_z = baseline_z_c - min_baseline_c;
            if (spread_z > min_rise_z) {
                /* ONE unbounded runtime float substitution (spread_z), not
                 * two -- -Wformat-truncation sizes EVERY %f substitution at
                 * its type's worst case (~40 bytes for an unbounded float)
                 * independently, so min_rise_z (also unbounded, a per-zone
                 * computed value, unlike SETTLE_ABS_SLOPE_FLOOR_C_PER_S in
                 * the stability refusal above, which is a single known
                 * compile-time constant and costs gcc nothing) has to stay
                 * out of the format string entirely, not just be a second
                 * "float substitution" by count -- two genuinely-unbounded
                 * floats blew the 96-byte estimate at build time even
                 * though the ACTUAL numbers here are always small. */
                if (reason) {
                    snprintf(reason, reason_cap,
                             "zone %u is %.1fC above the coolest zone -- not rested, let it settle",
                             (unsigned)z, (double)spread_z);
                }
                return false;
            }
        }
    }
    return true;
}
