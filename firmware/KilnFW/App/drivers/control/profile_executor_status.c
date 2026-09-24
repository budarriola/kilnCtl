/* profile_executor_halt()/pause()/resume()/get_status() and the small
 * read-only accessors (zone_is_active(), get_firing_history(),
 * get_history_count()/get_history()) -- split out of profile_executor.c
 * (2026-09-01, "files over 1500 lines should be broken up where it makes
 * sense"). See profile_executor_internal.h's own doc comment for the full
 * multi-way split this is one piece of. */

#include "profile_executor_internal.h"

#include <math.h>
#include <stdlib.h> /* free() -- firing-history blob is heap-allocated, see get_firing_history() */

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "adaptive_tune.h"
#include "heat_enable.h"
#include "relay_authority.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "zones_config_accessors.h"

void profile_executor_halt(void)
{
    /* See profile_executor_run()'s guard comment above -- s_exec.lock is
     * NULL until profile_executor_start() runs. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(PE_TAG, "profile_executor_halt() called before profile_executor_start() -- refused");
        return;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state == PROFILE_EXEC_IDLE) {
        xSemaphoreGive(s_exec.lock);
        return;
    }
    force_all_relays_off();
    /* An operator halt is an abnormal stop for the segment machinery too --
     * force off regardless of leave_on_at_end, same as a guard trip. An
     * operator stopping a firing on purpose is not the "reached its own
     * planned end" case that flag exists for. */
    io_segs_force_all_off(false);
    /* Hand every relay this run ever claimed back to unowned -- a halted run
     * owns nothing, and the next run (or a manual command) starts clean. */
    relay_authority_release_mask(s_exec.claimed_relay_mask);
    /* ...and give back the shared heat claim too (relay_authority.h) -- a
     * halt from RUNNING or PAUSED must free the whole-board sweep to start,
     * not just this run's relays. Not routed through release_profile_relay_
     * claim() (unlike every OTHER terminal transition) because that
     * function's single relay_authority_release_mask() call is this one's
     * near-duplicate, not something halt() can share without also pulling
     * in its own separate lock-held/state-transition assumptions -- calling
     * both here inline keeps halt() self-contained the way it already is.
     * Safe unconditionally, same no-op-if-never-held reasoning as
     * release_profile_relay_claim()'s call. */
    relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
    /* ...and the per-zone claim (relay_authority.h) too, for the same
     * "halt() is self-contained" reason the two calls above are inline
     * rather than routed through release_profile_relay_claim() -- see that
     * function's own matching call for why s_exec.profile.zone_mask is the
     * right mask and why this is safe unconditionally. */
    relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, s_exec.profile.zone_mask);
    /* ...and K4, inline for the same reason the two calls above are inline
     * rather than routed through release_profile_relay_claim(). After
     * force_all_relays_off() above, never before it. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    /* Captured BEFORE the fault fields are cleared below: if this halt is the
     * operator acknowledging a trip, the reason for that trip is the most
     * useful thing the breadcrumb can carry, and clearing it first would
     * throw it away. */
    run_snapshot_buf_t halt_snap;
    capture_run_snapshot(&halt_snap);
    profile_exec_state_t state_at_halt = s_exec.state;
    /* PID_EXPANSION_PLAN.md Phase 7a: the OTHER ending the tick loop's DONE/
     * FAULTED branch doesn't see -- an operator Stop straight out of RUNNING
     * or PAUSED. firing_stats_maybe_finalize()'s own fs_persisted/IDLE guard
     * makes this a no-op when state_at_halt is DONE/FAULTED (already
     * persisted by the tick loop above) or when this run never produced a
     * single tick worth persisting. Must happen before s_exec.state is reset
     * to IDLE just below -- firing_stats_build_record() reads s_exec.profile/
     * zones/total_elapsed_s, all still this run's values here. */
    bool fs_need_persist = false;
    profile_firing_run_record_t fs_rec;
    fs_need_persist = firing_stats_maybe_finalize(&fs_rec);
    clear_this_runs_faults();
    /* Routed through the same helper every other terminal transition uses
     * (profile_executor_relay_io.c) rather than assigning s_exec.state
     * directly -- 2026-09-24 halt-clear follow-up. Before this, a halt
     * straight out of RUNNING/PAUSED (mid-dwell, or mid-ramp-lock) left
     * dwelling/ramp_lock_held stale and every zone's `active` still true,
     * which rule 4/5's "Correction, audit 2026-09-24" comment (profile_
     * executor_internal.h) documents as unreachable-by-the-assert-today but
     * real, stale data -- this call is what makes it not exist in the first
     * place. The zone `active` clear happens on IDLE only (FAULTED/DONE keep
     * it), which is why this must stay AFTER force_all_relays_off(),
     * firing_stats_maybe_finalize() and clear_this_runs_faults() above --
     * each iterates active zones. Still under s_exec.lock here, the same
     * precondition the helper documents. */
    exec_enter_terminal_state(PROFILE_EXEC_IDLE);
    s_exec.fault_reason[0] = '\0';
    s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE;
    xSemaphoreGive(s_exec.lock);

    /* CommonFW/docs/LINK_PROTOCOL.md sec 4, SAFETY_CMD_SET_FIRING_CEILING
     * (0x09) -- clear the Pico's cached ceiling on every real stop (the
     * IDLE early-return above already refused a no-op halt, so reaching here
     * means a firing that was RUNNING/PAUSED/DONE/FAULTED just ended).
     * 0.0f is the documented "no firing / no ceiling known" sentinel
     * (LINK_PROTOCOL.md sec 4) -- S1 falls back to abs_max_temp_c alone,
     * which only ever LOOSENS the effective ceiling, never tightens it.
     * Outside s_exec.lock, same producer-call discipline as every other
     * safety_link send site in this file's siblings. */
    esp_err_t ceiling_err = safety_link_send_firing_ceiling(s_exec.safety, 0.0f);
    if (ceiling_err != ESP_OK) {
        ESP_LOGW(PE_TAG, "profile_executor_halt: SET_FIRING_CEILING clear failed (err=%s)",
                 esp_err_to_name(ceiling_err));
    }

    if (fs_need_persist) {
        firing_stats_persist(&fs_rec);
        /* PID_EXPANSION_PLAN.md Phase 7d: `clean` is always false here --
         * every run finalized on THIS path (see the comment above) is
         * either an operator stop straight out of RUNNING/PAUSED (stopped
         * early, by definition) or a dismiss of an already-persisted
         * DONE/FAULTED run (fs_need_persist is false for that case, so this
         * line is not reached at all -- see firing_stats_maybe_finalize()'s
         * fs_persisted guard). Never "true" from this call site. */
        adaptive_tune_run_end(&fs_rec, false);
    }

    /* An operator halt is a CLEAN end -- that is the whole point of recording
     * it. Without this write the record would still say RUNNING, and the next
     * boot would report a firing the operator deliberately stopped as one the
     * power cut short. A halt that acknowledges a latched trip keeps the
     * FAULTED phase instead, and dismissing a finished run keeps DONE: "a
     * guard stopped it" and "it ran to completion" are truer summaries of
     * those firings than "the operator stopped it", and halt() is also how
     * both of those states are dismissed from the dashboard. Only a halt out
     * of RUNNING/PAUSED is genuinely an operator stop. Either way the record
     * ends up marked ended, which is the property that matters. */
    run_state_phase_t end_phase = RUN_STATE_PHASE_HALTED;
    if (state_at_halt == PROFILE_EXEC_FAULTED) {
        end_phase = RUN_STATE_PHASE_FAULTED;
    } else if (state_at_halt == PROFILE_EXEC_DONE) {
        end_phase = RUN_STATE_PHASE_DONE;
    }
    run_state_note(end_phase, &halt_snap.snap);

    /* Natural end point for the contact-cycle counter: force a write now
     * rather than waiting out the 10-minute interval, so a firing's relay
     * wear survives a power-down right after it stops. Outside the lock --
     * relay_cycles.c takes its own. */
    esp_err_t flush_err = relay_cycles_flush();
    if (flush_err != ESP_OK) {
        /* No tick will retry this -- the executor is already halted -- so the
         * contact-cycle counts stay dirty in RAM only until the next periodic
         * relay_cycles_maybe_persist() from some other run, or a reboot loses
         * them. Log loudly rather than silently swallow it. */
        ESP_LOGW(PE_TAG, "relay_cycles_flush() failed at halt: %s -- cycle counts stay dirty in RAM",
                 esp_err_to_name(flush_err));
    }
    ESP_LOGI(PE_TAG, "profile executor halted");
}

bool profile_executor_pause(void)
{
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(PE_TAG, "profile_executor_pause() called before profile_executor_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state != PROFILE_EXEC_RUNNING) {
        xSemaphoreGive(s_exec.lock);
        return false;
    }
    force_all_relays_off();
    /* A pause gives K4 back, even though the relay claim is only handed to
     * MANUAL rather than released (see the comment just below). The two are
     * not the same question: keeping this run's relays reserved for a resume
     * costs nothing, whereas leaving the safety processor permitting heat
     * across a pause of unknown length -- possibly forever, if nobody ever
     * resumes -- means the ONE interlock that stands between a stuck relay
     * and a live element is held open by a firing that is not driving
     * anything. profile_executor_resume() re-acquires; heat_enable_acquire()
     * is a single frame, so nothing about that is expensive. Also consistent
     * with heater_output_force_off()'s own treatment of a pause as a full
     * de-energize that bypasses the min-on-time hold. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    /* TODO.md section 0: pausing is the one explicit way to hand a
     * PROFILE-owned relay back to MANUAL (not NONE -- a paused firing still
     * "belongs" to the operator's session, it's just not driving right now;
     * resuming reclaims PROFILE below). Chosen over auto-pause-on-touch so
     * a manual command never has the side effect of pausing a firing. */
    relay_authority_claim_mask(s_exec.claimed_relay_mask, RELAY_OWNER_MANUAL);
    s_exec.state = PROFILE_EXEC_PAUSED;
    run_snapshot_buf_t pause_snap;
    capture_run_snapshot(&pause_snap);
    xSemaphoreGive(s_exec.lock);

    /* PAUSED is recorded but is NOT an ending (see run_state.h): a firing
     * paused at 2am and never resumed because the power failed is still an
     * interrupted firing, and the operator deserves to be told so. Recording
     * it at all is what makes the segment progress accurate at the moment
     * the ramp/dwell clock stopped -- the periodic refresh is RUNNING-only. */
    run_state_note(RUN_STATE_PHASE_PAUSED, &pause_snap.snap);
    return true;
}

bool profile_executor_resume(void)
{
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(PE_TAG, "profile_executor_resume() called before profile_executor_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    if (s_exec.state != PROFILE_EXEC_PAUSED) {
        xSemaphoreGive(s_exec.lock);
        return false;
    }
    /* Reclaim PROFILE ownership handed to MANUAL on pause -- see
     * profile_executor_pause()'s comment. */
    relay_authority_claim_mask(s_exec.claimed_relay_mask, RELAY_OWNER_PROFILE);
    /* Re-ask for K4, released on pause -- see profile_executor_pause()'s
     * comment. Same "not a reason to refuse" handling as the start path.
     *
     * 2026-09-15 (review of 1c8d7f6e, finding MEDIUM-3): this used to run
     * right here, under s_exec.lock -- and since heat_enable_acquire() may
     * now itself flush a still-pending release (a blocking link exchange)
     * before its own enable=true exchange, that put up to TWO sequential
     * blocking link round-trips under this lock, which safety_poll_task
     * (via profile_executor_get_status()) and the HTTP status handlers all
     * block on every tick. Moved below the unlock: the state transition to
     * RUNNING and this call now happen in the same order as before from
     * every caller's point of view (state flips, then heat is asked for),
     * just without the lock held across the wire exchange. A request that
     * fails here is already tolerated -- see the start path's identical
     * comment -- heat_blocked/heat_block_sources report it and
     * heat_enable_reconcile() (watchdog task) retries. */
    /* Shared ramp/dwell state (target_c, segment_elapsed_s) is untouched by
     * pause -- the control task simply doesn't tick it while PAUSED, so
     * there's nothing to un-shift on resume (unlike the old tick-delta-
     * based timing this replaced, which needed to shift phase_start_tick by
     * the paused duration). prev_control_tick is reset so the next tick's
     * measured dt_s doesn't include the whole pause. */
    s_exec.prev_control_tick = xTaskGetTickCount();
    /* Bumpless transfer (TODO.md 6A.2): each active PID-mode zone resumes
     * as if it had been driving u=0 the whole pause (relays were off),
     * rather than an integral that jumps on the first post-resume tick.
     *
     * With feedforward on, u=0 is not reachable from a zero integral -- the
     * model contributes its hold duty the moment the loop runs again. So this
     * seeds the integral to 0 (see seed_bumpless_with_ff()) and the zone comes
     * back at exactly its feedforward duty: the model's own estimate of what
     * the setpoint costs to hold, with nothing accumulated on top. That is the
     * right place to restart from -- resuming a firing means resuming the heat
     * it needs -- and it is still bumpless in the sense that matters, no
     * integrator windup survives the pause. */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &s_exec.zones[zi];
        if (z->active && !z->faulted &&
            (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY) &&
            z->actual_valid) {
            seed_bumpless_with_ff(z, zi, 0.0f);
            z->fuzzy_prev_effective_ki = z->pid_cfg.ki; /* see reload_zone_config()'s same reasoning */
        }
    }
    s_exec.state = PROFILE_EXEC_RUNNING;
    run_snapshot_buf_t resume_snap;
    capture_run_snapshot(&resume_snap);
    /* Epoch sampled under s_exec.lock -- a halt landing between this unlock
     * and the acquire makes the acquire refuse instead of re-claiming heat
     * for a run that was just stopped. See heat_enable.h. */
    uint32_t he_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    xSemaphoreGive(s_exec.lock);
    (void)heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, he_epoch);

    /* Back to "in progress" -- and it must be written now rather than left to
     * the periodic refresh, or a brownout minutes after a resume would show
     * the firing as paused when it was actively driving elements. */
    run_state_note(RUN_STATE_PHASE_RUNNING, &resume_snap.snap);
    return true;
}

void profile_executor_get_status(profile_exec_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* See profile_executor_run()'s guard comment above -- this is the exact
     * call chain (safety_poll_task -> safety_build_and_send_context() ->
     * profile_executor_get_status()) that panicked on the bench. The zeroed
     * struct above already reads as a well-formed IDLE snapshot
     * (PROFILE_EXEC_IDLE == 0), so a caller here needs nothing more than
     * "don't touch the NULL lock". */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_status() called before profile_executor_start() -- reporting IDLE");
        return;
    }

    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    out->state = s_exec.state;
    /* Unconditional, unlike fault_reason/fault_guard below: a lifetime-of-
     * this-boot diagnostics counter, meaningful even once the board has
     * moved past the run that latched it (docs/audits/
     * profile_executor_panic_2026-09-24.md). */
    out->mode_state_violation_count = s_exec.mode_state_violation_count;
    if (s_exec.state != PROFILE_EXEC_IDLE) {
        out->profile_id = s_exec.profile_id;
        strncpy(out->profile_name, s_exec.profile.name, sizeof(out->profile_name) - 1);
        out->zone_mask = s_exec.profile.zone_mask;
        out->segment_index = s_exec.segment_index;
        out->segment_count = s_exec.profile.segment_count;
        out->dwelling = s_exec.dwelling;
        out->target_c = s_exec.target_c;
        out->segment_elapsed_s = s_exec.segment_elapsed_s;
        out->ramp_lock_held = s_exec.ramp_lock_held;
        out->ramp_lock_lagging_mask = s_exec.ramp_lock_lagging_mask;
        out->ramp_stretch_segment_s = (s_exec.segment_index < PROFILE_MAX_SEGMENTS)
                                           ? s_exec.stretch_by_segment_s[s_exec.segment_index]
                                           : 0.0f;
        out->ramp_stretch_total_s = s_exec.stretch_total_s;
        out->ramp_dwell_credit_applied_s = s_exec.dwell_credit_applied_s;
        out->run_start_c = s_exec.run_start_c;
        out->total_elapsed_s = s_exec.total_elapsed_s;
        out->warm_started = s_exec.warm_started;
        if (s_exec.warm_started) {
            strncpy(out->warm_start_reason, s_exec.warm_start_reason, sizeof(out->warm_start_reason) - 1);
            out->warm_start_replayed_count = s_exec.warm_start_replayed_count;
            memcpy(out->warm_start_replayed_segments, s_exec.warm_start_replayed_segments,
                   sizeof(out->warm_start_replayed_segments));
        }
        size_t seg_n = s_exec.profile.segment_count;
        if (seg_n > PROFILE_MAX_SEGMENTS) seg_n = PROFILE_MAX_SEGMENTS;
        memcpy(out->segments, s_exec.profile.segments, seg_n * sizeof(out->segments[0]));

        if (s_exec.dwelling) {
            const profile_segment_t *seg = &s_exec.profile.segments[s_exec.segment_index < s_exec.profile.segment_count
                                                                         ? s_exec.segment_index
                                                                         : s_exec.profile.segment_count - 1];
            /* PID_EXPANSION_PLAN.md sec 7.3: mirror the same dwell_credit_
             * applied_s subtraction the control tick's ready_to_advance
             * check uses (profile_executor.c), so this reported remaining
             * time agrees with when the schedule will actually advance --
             * 0.0f (no change) whenever the flag was off at this dwell's
             * entry, same bit-identical-with-flag-off guarantee. */
            uint32_t dwell_total_s = seg->dwell_min * 60u - (uint32_t)s_exec.dwell_credit_applied_s;
            out->dwell_remaining_s = s_exec.segment_elapsed_s >= dwell_total_s ? 0 : dwell_total_s - s_exec.segment_elapsed_s;
        }

        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            const zone_runtime_t *z = &s_exec.zones[zi];
            profile_exec_zone_status_t *zo = &out->zones[zi];
            zo->active = z->active;
            if (!z->active) continue;
            zo->actual_c = z->actual_c;
            zo->actual_valid = z->actual_valid;
            zo->relay_commanded_on = z->relay_commanded_on;
            zo->duty = z->duty;
            zo->control_mode = (uint8_t)z->control_mode;
            zo->faulted = z->faulted;
            if (z->faulted) {
                strncpy(zo->fault_reason, z->fault_reason, sizeof(zo->fault_reason) - 1);
                zo->fault_guard = (uint8_t)z->fault_guard;
            }
            zo->pid_p = z->last_pid_terms.p;
            zo->pid_i = z->last_pid_terms.i;
            zo->pid_d = z->last_pid_terms.d;
            zo->pid_ff = z->last_pid_terms.ff;
            zo->duty_breakdown = z->duty_breakdown; /* ROADMAP.md M15 B4, whole-struct copy */
            zo->cooling_limited = z->cooling_limited;
            zo->heat_blocked = z->heat_blocked;
            zo->heat_blocked_sources = z->heat_blocked_sources;
            zo->ff_hold_used_matrix = z->ff_hold_used_matrix;
            zo->ff_hold_infeasible = z->ff_hold_infeasible;
            zo->ff_climb_used_matrix = z->ff_climb_used_matrix;
            zo->ff_climb_infeasible = z->ff_climb_infeasible;
            zo->ff_membership_change_count = z->ff_membership_change_count;

            /* PID_EXPANSION_PLAN.md Phase 7a: live tracking-quality
             * snapshot, same derivation used for the persisted record --
             * see firing_stats_snapshot()'s doc comment. span uses the
             * run's current fs_target_min_c/max_c even mid-run, so this
             * live figure converges toward (but does not exactly equal, for
             * a still-narrowing/widening span) the final persisted one. */
            {
                float span = fabsf(s_exec.fs_target_max_c - s_exec.fs_target_min_c);
                if (isnan(s_exec.fs_target_min_c) || isnan(s_exec.fs_target_max_c)) {
                    span = 0.0f;
                }
                firing_stats_snapshot(z, span, &zo->firing_stats);
            }
            /* docs/audits/iter_tune_decision_2026-09-07.md prep -- live-only,
             * see profile_exec_zone_status_t.start_temp_c's own comment for
             * why this stays outside firing_stats_snapshot()/firing_stats
             * (that struct is also embedded in the persisted history blob). */
            zo->start_temp_c = z->fs_start_temp_c;

            /* PID_EXPANSION_PLAN.md sec 7.1/7.4: sustained-lag reporting,
             * ALWAYS (not gated on ramp_assist_enabled -- see profile_
             * executor.h's field comment). Commanded rate is this segment's
             * own signed ramp_c_per_hr (direction toward its target, same
             * sign convention the control loop's target_rate_c_per_s
             * uses); achieved is measured from lag_start_actual_c over the
             * time the lag has been held. Both 0 while not sustained. */
            zo->ramp_lag_sustained = z->lag_sustained;
            zo->ramp_lag_held_s = z->lag_held_s;
            zo->ramp_lag_commanded_rate_c_per_hr = 0.0f;
            zo->ramp_lag_achieved_rate_c_per_hr = 0.0f;
            if (z->lag_sustained && s_exec.segment_index < s_exec.profile.segment_count) {
                const profile_segment_t *lag_seg = &s_exec.profile.segments[s_exec.segment_index];
                if (lag_seg->seg_kind == PROFILE_SEG_KIND_ZONE_RAMP) {
                    float direction = (lag_seg->target_c >= s_exec.target_c) ? 1.0f : -1.0f;
                    zo->ramp_lag_commanded_rate_c_per_hr = direction * lag_seg->ramp_c_per_hr;
                    if (z->lag_held_s > 0.0f) {
                        zo->ramp_lag_achieved_rate_c_per_hr =
                            (z->actual_c - z->lag_start_actual_c) / (z->lag_held_s / 3600.0f);
                    }
                }
            }

            /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit, ALWAYS reported
             * (same convention as ramp_lag_* just above). */
            zo->ramp_dwell_credit_s = z->dwell_credit_s;
        }

        if (s_exec.state == PROFILE_EXEC_FAULTED) {
            strncpy(out->fault_reason, s_exec.fault_reason, sizeof(out->fault_reason) - 1);
            out->fault_guard = (uint8_t)s_exec.fault_guard;
            out->mode_state_fault_latched = s_exec.mode_state_fault_latched;
        }
    }
    xSemaphoreGive(s_exec.lock);
}

void profile_executor_get_live_status(profile_executor_live_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE(
            "profile_executor_get_live_status() called before profile_executor_start() -- reporting inactive");
        return;
    }

    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    out->active = (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED ||
                   s_exec.state == PROFILE_EXEC_FAULTED);
    if (out->active) {
        out->profile_id = s_exec.profile_id;
        out->segment_index = s_exec.segment_index;
    }
    out->has_refusal = s_exec.live_edit_last_refusal.valid;
    if (out->has_refusal) {
        out->refusal_generation = s_exec.live_edit_last_refusal.generation;
        out->refusal_result = (int)s_exec.live_edit_last_refusal.result;
        strncpy(out->refusal_err_msg, s_exec.live_edit_last_refusal.err_msg, sizeof(out->refusal_err_msg) - 1);
    }
    xSemaphoreGive(s_exec.lock);
}

bool profile_executor_zone_is_active(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_zone_is_active() called before profile_executor_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    bool active = (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) &&
                  (s_exec.profile.zone_mask & (1u << zone_index)) != 0;
    xSemaphoreGive(s_exec.lock);
    return active;
}

bool profile_executor_get_active_id(uint8_t *out_id)
{
    /* Narrow sibling of profile_executor_get_status() for callers that only
     * need "is a profile running/paused, and which one" -- e.g. the
     * benchproto PROFILES_CMD_DELETE handler in uart_bridge_ext_control.c,
     * which runs on bx_flash_worker's task stack (3792 B ceiling, zero
     * headroom on clean main). profile_exec_status_t is large enough that a
     * single stack-local instance of it there was itself the regression
     * (see git history); this avoids materializing that struct at all. Same
     * locking discipline as profile_executor_zone_is_active() just above:
     * no producer call under the lock, pure field reads only. */
    if (out_id) {
        *out_id = 0;
    }
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_active_id() called before profile_executor_start() -- reporting idle");
        return false;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    bool active = (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED);
    uint8_t id = s_exec.profile_id;
    xSemaphoreGive(s_exec.lock);
    if (active && out_id) {
        *out_id = id;
    }
    return active;
}

size_t profile_executor_get_firing_history(uint8_t profile_id, profile_firing_run_record_t *out,
                                            size_t max_entries)
{
    if (!out || max_entries == 0) {
        return 0;
    }
    /* NVS-only, no s_exec/lock -- see firing_stats_load()'s own doc comment.
     * Safe from any task/state, including before profile_executor_start(). */
    /* HEAP, not the stack (2026-09-08 panic, docs/audits/firing_history_
     * stack_overflow_2026-09-08.md): profile_firing_history_blob_t is 1364 B
     * and this function is called from firing_history_get_handler() on the
     * 8192 B httpd_worker stack, which was measured with 632-468 B free.
     * This frame plus firing_stats_load()'s / firing_stats_cfg_fs_resolve()'s
     * / _load_raw()'s own copies of the same blob summed to 6544 B of
     * statically-measured depth and overflowed the stack, smashing the TCB
     * (garbled exc_task, nonsense PC) on every GET /api/firing_history.
     * Internal DRAM, not PSRAM: firing_stats_load() reaches NVS/flash. */
    profile_firing_history_blob_t *blob =
        heap_caps_malloc(sizeof(*blob), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (blob == NULL) {
        ESP_LOGE(PE_TAG, "profile_executor_get_firing_history(%u): malloc(%u) failed -- reporting no history",
                 (unsigned)profile_id, (unsigned)sizeof(*blob));
        return 0;
    }
    if (!firing_stats_load(profile_id, blob)) {
        free(blob);
        return 0;
    }
    size_t n = blob->count;
    if (n > PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH) n = PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH; /* corrupt-blob guard */
    if (n > max_entries) n = max_entries;
    memcpy(out, blob->runs, n * sizeof(blob->runs[0])); /* runs[0] = newest, matches this function's contract */
    free(blob);
    return n;
}

uint32_t profile_executor_last_run_started_unix_s(uint8_t profile_id)
{
    /* RAM cache first (profile_executor_firing_stats.c's "last-run-started
     * RAM cache" section) -- GET /api/profiles calls this once per profile
     * id (PROFILES_MAX_COUNT + g_builtin_profile_count times per request),
     * and this used to mean a full firing_stats_load() NVS read every single
     * time, for every id, on every request. A hit here touches no NVS at
     * all; a miss falls through to the exact lookup this function always
     * did, then fills the cache so the NEXT request for this id is a hit. */
    uint32_t cached;
    if (firing_stats_cache_lookup(profile_id, &cached)) {
        return cached;
    }
    profile_firing_run_record_t rec;
    uint32_t started_unix_s = 0;
    if (profile_executor_get_firing_history(profile_id, &rec, 1) != 0) {
        started_unix_s = rec.run_started_unix_s;
    }
    firing_stats_cache_store(profile_id, started_unix_s);
    return started_unix_s;
}

size_t profile_executor_get_history_count(void)
{
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_history_count() called before profile_executor_start() -- refused");
        return 0;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    size_t count = s_exec.history_count;
    xSemaphoreGive(s_exec.lock);
    return count;
}

size_t profile_executor_get_history(profile_history_entry_t *out, size_t start_index, size_t max_entries)
{
    if (!out || max_entries == 0) {
        return 0;
    }
    /* See profile_executor_run()'s guard comment above. */
    if (s_exec.lock == NULL) {
        LOG_PRESTART_ONCE("profile_executor_get_history() called before profile_executor_start() -- refused");
        return 0;
    }
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    /* history_count only ever increments when s_exec.history is non-NULL
     * (profile_executor.c's sample site checks first), so this NULL check
     * is defensive, not load-bearing -- but a PSRAM allocation failure
     * (history_buf_ensure_alloc()) is exactly the case where being wrong
     * about that would deref NULL. */
    if (s_exec.history == NULL || start_index >= s_exec.history_count) {
        xSemaphoreGive(s_exec.lock);
        return 0;
    }
    size_t count = s_exec.history_count - start_index;
    if (count > max_entries) {
        count = max_entries;
    }
    /* Oldest-first: history_head is the next WRITE slot, so the oldest
     * valid entry (once the buffer has wrapped) is exactly history_head;
     * before it wraps, the oldest is index 0. start_index is relative to
     * that chronological ordering, not the raw array index. */
    uint16_t oldest = (s_exec.history_count < HISTORY_MAX_SAMPLES) ? 0 : s_exec.history_head;
    for (size_t i = 0; i < count; i++) {
        uint16_t idx = (uint16_t)((oldest + start_index + i) % HISTORY_MAX_SAMPLES);
        history_unpack(&s_exec.history[idx], &out[i]);
    }
    xSemaphoreGive(s_exec.lock);
    return count;
}
