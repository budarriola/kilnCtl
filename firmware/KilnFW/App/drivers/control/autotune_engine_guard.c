#include "autotune_engine_internal.h"
#include "heat_enable.h"
#include "profile_feasibility.h"

/* Shared low-level plumbing (relay apply/release, escalate/abort, threshold
 * scaling, trace unpack, "is a run in progress") plus the two persistence
 * entry points, autotune_engine_abort()/accept(). See autotune_engine_
 * internal.h's top comment for the full five-way split this file is one
 * piece of. */

/* profile_feasibility.h's FEASIBILITY_TUNING_METHOD_STEP is a literal copy
 * of AUTOTUNE_METHOD_STEP (autotune_engine.h) kept out of that module to
 * avoid pulling in this whole state machine for one enumerator -- pinned
 * here, in the one .c that already includes both, so the two cannot drift
 * silently. Portable compile-time assert (not _Static_assert): this file is
 * #included directly by test_autotune_engine_prestart.c's host-test
 * executable, which (unlike most of this repo's host-test executables) is
 * NOT built with /std:c11 -- see kiln_cfg_store.c's identical-pattern
 * comment for the same reasoning. A negative array size is a hard error in
 * every C standard this file has ever been built under. */
typedef char autotune_method_step_matches_feasibility_check
    [(AUTOTUNE_METHOD_STEP == FEASIBILITY_TUNING_METHOD_STEP) ? 1 : -1];

float autotune_scale_threshold_c(float base_c, float probe_k_rough)
{
    if (!(probe_k_rough > 0.0f)) {
        return base_c;
    }
    return base_c * (probe_k_rough / AUTOTUNE_REFERENCE_K_C_PER_DUTY);
}

bool state_is_running(autotune_engine_state_t s)
{
    return s == AUTOTUNE_ENGINE_SETTLING || s == AUTOTUNE_ENGINE_STEPPING ||
           s == AUTOTUNE_ENGINE_RELAY_APPROACH || s == AUTOTUNE_ENGINE_RELAY_CYCLING;
}

uint32_t at_ticks_to_s(TickType_t t) { return (uint32_t)(t / configTICK_RATE_HZ); }
uint32_t at_ticks_to_ms(TickType_t t) { return (uint32_t)t * (1000u / configTICK_RATE_HZ); }

/* Review blocker 1: cross-zone coupling can latch step_element_proven on a
 * DEAD element. Measured off-diagonal coupling on this hardware is 5-12
 * degC per unit duty -- a neighbour zone firing at duty 0.5 puts 2.5-6 degC
 * into the zone under test, comfortably past AUTOTUNE_ELEMENT_ALIVE_RISE_C
 * (3.0), and step_element_proven's own check (autotune_engine_tick_locked())
 * has no way to tell "this zone's own element moved the plant" from "a
 * neighbour's did" -- it only reads this zone's raw rise. Once latched on
 * neighbour heat, guard 1's rise requirement relaxes on an element that has
 * done nothing, guard 2 is structurally unreachable during a step test (see
 * that mechanism's own comment), and guards 4/7 do not apply either -- a
 * dead element then runs the full budget with no progress guard at all.
 *
 * FIX CHOICE: refuse to START a STEP run (both autotune_engine_run() and
 * autotune_engine_run_to_target()) while ANY OTHER zone has an active
 * profile, and ABORT one already running if another zone's profile starts
 * mid-run (autotune_engine_tick_locked() calls this too, STEP method only,
 * right alongside the whole-run budget check). REJECTED alternative:
 * "require the rise be attributable to this zone's own commanded duty" --
 * that needs a trusted coupling model to subtract the neighbour's expected
 * contribution, and the coupling matrix is exactly what an autotune run
 * (TODO.md 6A.5(b)'s cross-gain fit) is used to MEASURE in the first place;
 * gating the safety of the measurement on the thing being measured is
 * circular. Requiring isolation is strictly stronger, simpler to verify,
 * and costs nothing an operator running a real characterisation session
 * wants anyway -- TODO.md 6A.5's own "one at a time" convention already
 * applies between two AUTOTUNE runs, this just extends it to "and no
 * profile on any other zone either" for the STEP method specifically,
 * where step_element_proven actually lives. Not applied to the RELAY
 * method, which has no such latch and already runs the full, unrelaxed
 * guard suite regardless of what other zones are doing.
 *
 * Loops profile_executor_zone_is_active() (already published, per-zone)
 * over every zone but the one under test -- no profile_executor.c change
 * needed; there is no "any zone" query published, and this file must not
 * add one to that translation unit while target-mode work is still
 * uncommitted and profile_executor.c has moved on independently. */
bool any_other_zone_profile_active(uint8_t zone_index)
{
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        if (z == zone_index) continue;
        if (profile_executor_zone_is_active(z)) {
            return true;
        }
    }
    return false;
}


/* Must be called with s_at.lock held. */
void autotune_apply_relay(bool want_on)
{
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(s_at.zone_index, &mask) || mask == 0) {
        s_at.duty = 0.0f;
        return;
    }
    /* Both outcomes below used to be discarded silently, and between them they
     * made a step test that never energized anything indistinguishable from
     * one that did: the engine reported duty 0.50 throughout (s_at.duty is set
     * from want_relay_on, not from what the relay did), the trace stayed flat
     * because no heat was ever applied, and the run ended "fit failed:
     * response too small to fit (trace flat or noise-dominated)" -- an
     * accusation against the kiln for the engine's own inaction. Observed on
     * the bench: 293 relay samples over 12 s of stepping, every one open, with
     * relay_cycles unmoved. profile_executor.c's autotune_apply_relay() already logs
     * both of these; this one did not. */
    if (want_on) {
        uint32_t sources = 0;
        if (relay_authority_zone_blocked(s_at.safety, s_at.zone_index, &sources)) {
            ESP_LOGW(AT_TAG, "autotune zone %u wants heat but is BLOCKED: sources 0x%02X -- the trace "
                          "will be flat and the fit will fail for that reason, not the kiln's",
                     s_at.zone_index, (unsigned)sources);
            want_on = false;
        }
    }
    if (s_at.io) {
        /* AUTHORIZED, not the manual gate -- see kiln_io_owner.h's top
         * comment and profile_executor.c's autotune_apply_relay() for the identical
         * reasoning (2026-08-19, TODO.md 10.14 Phase 1). */
        esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(mask, want_on ? mask : 0);
        if (err != ESP_OK) {
            ESP_LOGW(AT_TAG, "autotune zone %u relay write failed: %s -- relay state is unknown and "
                          "the trace cannot be trusted",
                     s_at.zone_index, esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(AT_TAG, "autotune zone %u has no expander handle -- nothing will be energized",
                 s_at.zone_index);
    }
    sim_backend_note_zone_relay(s_at.zone_index, want_on); /* no-op unless CONFIG_KILNCTL_SIM_PLANT */
}

/* Must be called with s_at.lock held. */
void force_relays_off(void)
{
    heater_output_force_off(&s_at.heater_state);
    autotune_apply_relay(false);
    s_at.duty = 0.0f;
    /* TODO.md 6A.6: release the RELAY_OWNER_AUTOTUNE claim autotune_begin_run_locked()
     * took on this zone's mask. force_relays_off() is the one function every
     * terminal path (autotune_finalize_fit(), finalize_relay_fit(),
     * autotune_escalate_and_abort(), abort_locked()) calls before leaving the running
     * states, so it's the single place the claim can be released without
     * duplicating this at each call site. A relay no longer in the zone's
     * mask (reconfigured mid-test) is simply not released here -- harmless,
     * since relay_authority_claim_mask() only ever wrote owners for the bits
     * that were in the mask it was given. */
    uint8_t owned_mask = 0;
    if (zones_config_get_relay_mask(s_at.zone_index, &owned_mask) && owned_mask != 0) {
        relay_authority_release_mask(owned_mask);
    }
    /* The per-zone claim (relay_authority.h) taken atomically in
     * autotune_begin_run_locked(), right before the whole-board heat claim
     * just below -- see that call site's own doc comment for why it exists
     * and why it must be released here too. Safe unconditionally, same
     * no-op-if-never-held reasoning as the calls above/below: clearing a bit
     * already clear changes nothing. */
    relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, (uint8_t)(1u << s_at.zone_index));
    /* The shared heat claim (relay_authority.h) taken atomically in
     * autotune_begin_run_locked(), right after state_is_running() confirmed this was
     * a genuine start. Safe unconditionally: a no-op if this run never
     * actually reached that point (refused earlier). */
    relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE);
    /* ...and give K4 back (heat_enable.h). Same single-funnel reasoning as
     * the two releases above: autotune_finalize_fit(), finalize_relay_fit(),
     * autotune_escalate_and_abort() and abort_locked() all call this function before
     * leaving a running state, so this is the one place the autotune's
     * heat-enable request is torn down. Deliberately AFTER autotune_apply_relay(false)
     * at the top of this function -- dropping the zone relay is never gated
     * on giving K4 back. Idempotent, so a path that never acquired (a
     * refused start) sends nothing. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
}

/* Same escalation split as profile_executor.c's escalate_guard_trip() --
 * duplicated rather than shared because the two modules' state structs
 * differ enough that a shared helper would need to take 4+ out-params; see
 * TODO.md 6A.6 for the policy this mirrors. */
void autotune_escalate_and_abort(thermal_guard_trip_t reason, const char *detail)
{
    bool global = (reason == THERMAL_GUARD_TRIP_RUNAWAY || reason == THERMAL_GUARD_TRIP_MAX_TEMP ||
                  reason == THERMAL_GUARD_TRIP_MIN_TEMP || reason == THERMAL_GUARD_TRIP_SENSOR_INVALID);
    uint32_t source = (reason == THERMAL_GUARD_TRIP_SENSOR_INVALID) ? SAFETY_FAULT_SRC_THERMO
                                                                    : SAFETY_FAULT_SRC_THERMAL_SANITY;
    if (global) {
        if (s_at.safety) {
            safety_link_set_fault_source(s_at.safety, source, true);
        }
        /* profile_executor.c's escalate_guard_trip() records the same thing in
         * global_fault_source so clear_this_runs_faults() knows what to clear
         * later. Before this, autotune_engine had no such bookkeeping and no
         * clearing path at all: a global guard trip during an autotune left
         * this source asserted board-wide until reboot, which
         * relay_authority_on_blocked() then read as a reason to refuse every
         * relay-ON everywhere -- other zones, profiles, and later autotunes
         * alike -- and which also masked whatever different fault came next,
         * since the mask never returned to clean. See the clearing next to
         * relay_authority_zone_latched_blocked() in autotune_begin_run_locked(). */
        s_at.global_fault_source = source;
    } else {
        relay_authority_set_zone_blocked(s_at.zone_index, true);
        s_at.per_zone_blocked = true;
    }
    force_relays_off();
    s_at.state = AUTOTUNE_ENGINE_ABORTED;
    /* Explicit precision (sizeof(abort_reason) - strlen("guard tripped: ") -
     * 1) rather than a bare %s -- under -Os this call started inlining into
     * task_entry, and GCC's -Wformat-truncation can't bound an unadorned %s
     * against a caller-supplied `detail` even though the destination is
     * fixed-size; found building 2026-08-18 (the -Og build never triggered
     * this since the call stayed out-of-line there). Genuinely truncating an
     * overlong detail string here is fine -- abort_reason is a status
     * readout, not parsed anywhere -- this only silences a false positive. */
    snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "guard tripped: %.79s", detail);
    ESP_LOGE(AT_TAG, "autotune zone %u aborted: %s", s_at.zone_index, s_at.abort_reason);
}

void clear_block_if_any(void)
{
    if (s_at.per_zone_blocked) {
        relay_authority_set_zone_blocked(s_at.zone_index, false);
        s_at.per_zone_blocked = false;
    }
}

/* The single non-guard abort path, extracted from autotune_engine_abort() so
 * the relay method's own aborts (approach budget expired, unusable trace)
 * cannot drift away from what the operator's Abort button does. Guard trips
 * still go through autotune_escalate_and_abort() instead, because those additionally
 * have to raise a fault source or block the zone -- an abort that *found
 * something wrong with the kiln* must not clear itself the way a cancelled
 * test does. Must be called with s_at.lock held, and only from a running
 * state. */
void abort_locked(const char *reason)
{
    force_relays_off();
    clear_block_if_any();
    thermal_guard_clear(&s_at.guard_state);
    s_at.state = AUTOTUNE_ENGINE_ABORTED;
    snprintf(s_at.abort_reason, sizeof(s_at.abort_reason), "%s", reason ? reason : "aborted");
}

/* The trace is stored packed (see autotune_engine.h) but pid_autotune.c is
 * pure math over {t_s, measurement_c} pairs and has no business knowing that.
 * Unpacking one zone's row into a scratch buffer for the duration of a fit
 * costs ~11 KB transiently, which is affordable *because* the packing freed
 * far more than that permanently -- and it is on the heap rather than this
 * task's 4 KB stack for the obvious reason. Returns NULL on allocation
 * failure; the caller aborts rather than fitting a partial trace. */
autotune_sample_t *autotune_unpack_zone_trace(uint8_t zone, size_t count)
{
    if (count == 0) {
        return NULL;
    }
    autotune_sample_t *out = malloc(count * sizeof(*out));
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        out[i].t_s = (float)(i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        int16_t dc = s_at.zone_trace[zone][i];
        out[i].measurement_c = (dc == AUTOTUNE_TRACE_TEMP_INVALID) ? NAN : (float)dc / 10.0f;
    }
    return out;
}

void autotune_engine_abort(const char *reason)
{
    /* See autotune_begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        ESP_LOGW(AT_TAG, "autotune_engine_abort() called before autotune_engine_start() -- refused");
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (!state_is_running(s_at.state)) {
        xSemaphoreGive(s_at.lock);
        return;
    }
    abort_locked(reason);
    xSemaphoreGive(s_at.lock);
    ESP_LOGI(AT_TAG, "autotune zone %u aborted by request: %s", s_at.zone_index, s_at.abort_reason);
}


bool autotune_engine_accept(const autotune_accept_opts_t *opts, autotune_accept_result_t *out)
{
    autotune_accept_opts_t effective_opts = {0};
    if (opts != NULL) {
        effective_opts = *opts;
    }
    bool ack_unsettled = effective_opts.ack_unsettled;
    bool adopt_ceiling = effective_opts.adopt_ceiling;

    if (out != NULL) {
        out->adoption = AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED;
        out->old_ceiling_c_per_hr = 0.0f;
        out->new_ceiling_c_per_hr = 0.0f;
        out->refused_by_mode_gate = false;
        out->mode_reason[0] = '\0';
    }
    /* Owner decision 2026-10-08 (docs/SYSTEM_MODE_GATE_PLAN.md): accept
     * writes zone PID gains/max_ramp, so refuse while a firing or autotune
     * run is active, before any mutation. Single choke point for the HTTP
     * and UART-bridge callers. */
    {
        sys_mode_snapshot_t mode_snap = {0};
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(AT_TAG, "autotune_engine_accept() refused by system mode gate: %s", mode_reason);
            if (out != NULL) {
                out->refused_by_mode_gate = true;
                snprintf(out->mode_reason, sizeof(out->mode_reason), "%s", mode_reason);
            }
            return false;
        }
    }
    /* See autotune_begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        ESP_LOGW(AT_TAG, "autotune_engine_accept() called before autotune_engine_start() -- refused");
        return false;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    bool have_result = (s_at.method == AUTOTUNE_METHOD_RELAY) ? s_at.relay.valid : s_at.model.valid;
    if (s_at.state != AUTOTUNE_ENGINE_DONE || !have_result) {
        xSemaphoreGive(s_at.lock);
        return false;
    }
    /* 2026-09-01 review fix, extended 2026-09-02 (round-3 follow-up): the
     * ack_unsettled gate now covers all three "is this fit fully
     * trustworthy" signals fopdt_model_t carries -- fopdt_model_t::settled
     * (the STEPPING-phase relative-slope detector), ::extrapolation_
     * converged and ::tau_consistent_with_gain (pid_autotune_fit_fopdt()'s
     * asymptote-correction loop) -- not just settled. The latter two were
     * correctly populated and then ignored, the exact write-only pattern
     * the settled review already caught once -- see fopdt_model_t's own
     * KNOWN OPEN GAP comment (pid_autotune.h) for the full history. ONE
     * acknowledgement path (ack_unsettled) covers all three conditions;
     * the refusal text below names exactly which one(s) are unmet so an
     * operator (or a caller reading the reason) can tell WHY, not just
     * THAT -- see also autotune_build_status()'s wire fields and the
     * dashboard JSON, which surface all three distinctly rather than
     * collapsing them into one bit. */
    if (s_at.method == AUTOTUNE_METHOD_STEP) {
        bool settled = s_at.model.settled;
        bool converged = s_at.model.extrapolation_converged;
        bool tau_ok = s_at.model.tau_consistent_with_gain;
        if ((!settled || !converged || !tau_ok) && !ack_unsettled) {
            xSemaphoreGive(s_at.lock);
            /* Final review fix: widened 112->160 (the three current reason
             * strings need 123 bytes concatenated -- 112 left only a
             * handful of bytes of margin, not overflow today only because
             * this happens to be the last write) and the accumulation
             * below is now underflow-safe regardless of buffer size or how
             * many conditions are ever added: `o` is clamped to
             * sizeof(reasons) before every `sizeof(reasons) - o`, so that
             * subtraction can never wrap a size_t into a huge value and
             * hand snprintf a bogus "room" figure. Without the clamp, a
             * FOURTH condition added later would push `o` past
             * sizeof(reasons) and the very next `sizeof(reasons) - o`
             * would underflow -- exactly the defect being fixed here
             * before it exists, not after. */
            char reasons[160];
            size_t o = 0;
            reasons[0] = '\0';
            if (!settled) {
                size_t room = (o < sizeof(reasons)) ? sizeof(reasons) - o : 0;
                o += (size_t)snprintf(reasons + o, room, "never genuinely settled "
                                                          "(max-duration backstop)");
                if (o > sizeof(reasons)) o = sizeof(reasons);
            }
            if (!converged) {
                size_t room = (o < sizeof(reasons)) ? sizeof(reasons) - o : 0;
                o += (size_t)snprintf(reasons + o, room, "%sextrapolation did not converge", o ? "; " : "");
                if (o > sizeof(reasons)) o = sizeof(reasons);
            }
            if (!tau_ok) {
                size_t room = (o < sizeof(reasons)) ? sizeof(reasons) - o : 0;
                o += (size_t)snprintf(reasons + o, room, "%stau/L inconsistent with the corrected gain",
                                      o ? "; " : "");
                if (o > sizeof(reasons)) o = sizeof(reasons);
            }
            ESP_LOGW(AT_TAG, "autotune zone %u: accept refused -- %s; pass ack_unsettled=true to accept "
                          "anyway",
                     s_at.zone_index, reasons);
            return false;
        }
    }
    /* iter_tune_http.c restore_commissioned race close, follow-up (step-7
     * review, 2026-09-23; opus round 2): a DONE run's own accept() writes
     * gains via the same zones_config_set_pid() the restore handler calls --
     * reaching accept() from a different task (uart_bridge_ext_autotune.c's
     * benchproto ACCEPT command) while iter_tune_http.c holds a reservation
     * on this zone would race the restore exactly the way autotune_begin_
     * run_locked()'s check above already prevents a fresh START from doing.
     * Checked under the SAME lock, at the SAME "we are about to commit"
     * point autotune_begin_run_locked() uses -- see that function's and
     * s_at_t's own comments for the reservation's lock discipline.
     *
     * Round 2 fix: the first version of this gate only READ the flag here
     * and released the lock at xSemaphoreGive(s_at.lock) below with nothing
     * held across the zones_config_set_pid() write further down -- leaving
     * the exact window open that iter_tune_http.c's own reservation exists
     * to close (a restore's reserve() could land between this check and
     * that write, and both would then write the zone's gains). accept() now
     * takes the SAME reservation slot for its own zone right here, under
     * this lock hold, so a concurrent reserve() from the restore handler is
     * refused for as long as accept() is still writing -- symmetric with
     * how a reservation already refuses a fresh START. Released via
     * autotune_engine_release_zone_for_external_write() on EVERY exit path
     * below (the early failure return once zones_config_set_pid() is
     * called, and both success returns), never left held past this call. */
    if (s_at.external_write_reserved && s_at.external_write_reserved_zone == s_at.zone_index) {
        xSemaphoreGive(s_at.lock);
        ESP_LOGW(AT_TAG, "autotune zone %u: accept refused -- a gain write is in progress on "
                          "this zone",
                 s_at.zone_index);
        return false;
    }
    s_at.external_write_reserved = true;
    s_at.external_write_reserved_zone = s_at.zone_index;

    uint8_t zone = s_at.zone_index;
    autotune_method_t method = s_at.method;
    autotune_gains_t g = s_at.proposed_gains;
    fopdt_model_t m = s_at.model;
    /* Captured here, under the same lock as everything else above, for the
     * tuning-quality record written below -- see zones_config_set_tuning_
     * quality()'s own comment (zones_http.h) for why this belongs alongside
     * the model/gains rather than re-read from s_at after the lock is
     * released (a new run could already be starting by then). */
    float step_ambient_c = s_at.step_ambient_c;
    float predicted_max_ramp_ambient_c_per_hr = s_at.predicted_max_ramp_ambient_c_per_hr;
    xSemaphoreGive(s_at.lock);

    /* Review finding 1b (cbc43a7f): the mode-gate snapshot at the top of this
     * function predates s_at.lock and the reservation, so a profile start on
     * another task can land in that window. Re-check now that the reservation
     * is held and before the first write. relay_authority_heat_run_active()
     * is a leaf spinlock (takes no other lock), so calling it here -- after
     * s_at.lock was released, holding only the reservation flag -- cannot
     * invert any lock order. A start that lands after this re-check is not
     * closed here: profile_executor_run() has no cheap way to see the
     * reservation, so a residual window of the few writes below remains.
     * Other gated writers (e.g. zones_http_pid.c) share the same
     * snapshot-then-write pattern; deliberately unchanged in this pass. */
    {
        sys_mode_snapshot_t recheck = {0};
        relay_authority_heat_run_active(&recheck.profile_running, &recheck.autotune_running);
        char recheck_reason[SYSTEM_MODE_GATE_REASON_MAX];
        recheck_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &recheck, recheck_reason,
                                   sizeof(recheck_reason))) {
            autotune_engine_release_zone_for_external_write(zone);
            ESP_LOGW(AT_TAG, "autotune_engine_accept() refused by system mode gate (re-check): %s",
                     recheck_reason);
            if (out != NULL) {
                out->refused_by_mode_gate = true;
                snprintf(out->mode_reason, sizeof(out->mode_reason), "%s", recheck_reason);
            }
            return false;
        }
    }

    if (!zones_config_set_pid(zone, g.kp, g.ki, g.kd)) {
        autotune_engine_release_zone_for_external_write(zone);
        return false;
    }

    /* Q3: this IS the "re-autotune this zone" the adaptive_tune Ki-diagnosis
     * cumulative-bound refusal names as its remedy -- an autotune result was
     * just committed for this zone (gains just persisted above, unconditional
     * of method). Clear its stale Ki-diagnosis baseline here so the next time
     * that layer reaches a live Ki for this zone it re-latches fresh, rather
     * than staying capped against a ceiling derived from before this re-tune.
     * See adaptive_tune_clear_ki_baseline()'s own comment for the lock-order
     * reasoning on why this call belongs AFTER s_at.lock was already released
     * above (xSemaphoreGive(s_at.lock)), never before.
     *
     * RE-ENTRANCY, not just lock order: when this accept path is reached over
     * the UART bridge (uart_bridge_ext.c AUTOTUNE_CMD_ACCEPT), autotune_
     * engine_accept() is ALREADY running on the flash-safe worker task (see
     * that file's HAZARD block, "autotune_task: autotune_engine_accept()").
     * adaptive_tune_clear_ki_baseline() itself dispatches onto that same
     * worker via uart_bridge_ext_run_on_flash_worker() to persist the
     * cleared baseline -- a naive dispatch from here would deadlock the
     * worker permanently (non-recursive lock held by the original caller,
     * 1-deep queue only the now-blocked worker drains; a recursive mutex
     * would not save it either). Fixed at the source: bx_run_on_internal_
     * stack() now detects "caller is already the worker task" and runs the
     * job inline instead of dispatching, since the worker's own stack is
     * already internal SRAM and the PSRAM/flash-cache hazard is already
     * satisfied. The HTTP accept path (dashboard_http.c, httpd task) is
     * unaffected -- it was never on the worker to begin with.
     *
     * profile_executor_halt() (reached on-worker via uart_bridge_ext.c's own
     * on-worker list) also reaches adaptive_tune_run_end(), which has the
     * identical mechanism and the identical uart_bridge_ext_is_on_flash_
     * worker() guard -- but that path is not actually re-entrant today:
     * profile_executor_status.c hardcodes `clean=false` at its call site,
     * which forecloses baseline_newly_latched and so never dispatches.
     * Defensive only, kept in case `clean` stops being a constant -- see
     * that function's comment in adaptive_tune.c. This accept path remains
     * the one genuinely reachable re-entrant caller. */
    adaptive_tune_clear_ki_baseline(zone);

    if (method == AUTOTUNE_METHOD_RELAY) {
        /* A relay test measures ONE point of the frequency response: the gain
         * and period at which the loop's phase is -180 degrees. It does not
         * measure K, tau or L, and no combination of (Ku, Tu) recovers them --
         * infinitely many FOPDT plants share any given ultimate gain and
         * period. So there is nothing to write through zones_config_set_model()
         * here, and equally importantly nothing to OVERWRITE: if a previous
         * step test measured this zone's model, that model is still the best
         * (and only) thing 6A.2's feedforward and the ramp-ceiling estimate
         * have to work from. Clearing it, or synthesising one from Ku, would
         * trade a measurement for a guess and would do it silently.
         *
         * The gains above are the entire result of this run. */
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        clear_block_if_any();
        s_at.state = AUTOTUNE_ENGINE_IDLE;
        xSemaphoreGive(s_at.lock);
        ESP_LOGI(AT_TAG, "autotune zone %u: relay-test gains accepted (no plant model written -- a relay test "
                      "measures none; any model from a previous step test is left untouched)", zone);
        if (adopt_ceiling && out) out->adoption = AUTOTUNE_CEILING_SKIPPED_RELAY_METHOD;
        autotune_engine_release_zone_for_external_write(zone);
        return true;
    }
    /* The model goes with the gains, through the same owner and at the same
     * moment (TODO.md 6A.4: results are proposed, never auto-applied, and
     * zones_http stays the one owner of zone config -- this engine must not
     * touch NVS itself). 6A.2's feedforward is computed from K and tau, so
     * a run whose gains were accepted but whose model was dropped would
     * leave the loop tuned but unable to feed forward, which is the half of
     * the improvement that actually keeps a ramp on the curve.
     *
     * A failure here is logged, not propagated: the gains above are already
     * live and persisted, and returning false would tell the operator the
     * acceptance failed when the thing they clicked Accept for did in fact
     * land. The consequence of the missing model is a zone that runs on
     * feedback alone -- exactly how every zone ran before this existed --
     * and it is recoverable by re-running the test. Losing the accepted
     * gains to a false return would not be. */
    bool model_persisted = zones_config_set_model(zone, m.k_gain_c_per_duty, m.tau_s, m.dead_time_s);
    if (!model_persisted) {
        ESP_LOGW(AT_TAG,
                 "autotune zone %u: gains accepted but plant model (K=%.2f tau=%.1f L=%.1f) was rejected or "
                 "failed to persist -- feedforward will stay off for this zone",
                 zone, (double)m.k_gain_c_per_duty, (double)m.tau_s, (double)m.dead_time_s);
    } else {
        /* docs/audits/high_temperature_transfer_analysis_2026-09-08.md item
         * 1: record the operating point this fit was actually taken at, from
         * the same measured values the tuning-quality record just below uses
         * (m.baseline_c -- the real measured temperature the step began
         * from, and step_ambient_c -- the real measured ambient at that
         * moment), never from the setpoint the run was driving toward. Gated
         * on model_persisted for the same reason coupling_diag_k_dc/the
         * ceiling adoption below are: writing a fit context for a model that
         * did NOT land would describe a fit that, as far as this zone's
         * config is concerned, never happened. Best-effort like the model
         * write's own failure path -- a rejected context leaves the model
         * usable but its operating point unrecorded (same as any pre-v24
         * record), not a reason to undo the model or gains that already
         * landed. */
        if (!zones_config_set_model_fit_context(zone, m.baseline_c, step_ambient_c)) {
            ESP_LOGW(AT_TAG,
                     "autotune zone %u: plant model persisted but its fit context (T=%.1f ambient=%.1f) was "
                     "rejected or failed to persist -- this fit's operating point will read as unknown",
                     zone, (double)m.baseline_c, (double)step_ambient_c);
        }
        /* docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md item
         * 4 ("reset one side of a pair"): a NEW full autotune Accept just
         * replaced this zone's plant model from a fresh, trustworthy
         * measurement -- re-anchor adaptive_tune_model.c's ratchet-prevention
         * baseline (autotune_baseline_k_dc) to THIS value, right alongside
         * the model write it anchors, not on some later, easier-to-forget
         * path. Joined by the SAME event to adaptive_tune_clear_ki_baseline()
         * just above (called unconditionally for both methods, before this
         * `else` branch): that call already re-latches the Ki-diagnosis side
         * of this same "a fresh autotune result landed" contract every time
         * gains are accepted; this call re-latches the K_dc side every time a
         * MODEL is accepted (a strict subset -- relay-method runs write gains
         * but no model, see this function's own comment above, so they
         * correctly clear Ki's baseline without touching this one). Letting
         * only one of the two re-anchor would leave the other comparing
         * future refinements against a now-stale reference from before this
         * autotune -- exactly the "reset one side of a pair" bug class this
         * codebase has shipped four times before. Best-effort like every
         * other write in this block: a failed persist here leaves the OLD
         * baseline in place, which is safe (adaptive_tune_refine_zone_
         * locked() will just keep bounding drift against a slightly stale
         * anchor, never against nothing) -- not a reason to undo the model or
         * gains that already landed. */
        if (!zones_config_set_autotune_baseline_k_dc(zone, m.k_gain_c_per_duty)) {
            ESP_LOGW(AT_TAG,
                     "autotune zone %u: plant model persisted but its adaptive-tune baseline K_dc=%.4f was "
                     "rejected or failed to persist -- adaptive_tune will keep bounding drift against the "
                     "prior (now stale) baseline until this succeeds",
                     zone, (double)m.k_gain_c_per_duty);
        }
    }
    /* TODO.md 6A.4, "autotune's predicted ramp ceiling is shown but not
     * wired into max_ramp_c_per_hr" -- opt-in only (adopt_ceiling defaults
     * false everywhere: autotune_engine_accept() above, the HTTP handler,
     * and the UART bridge), so an operator who has not asked for it sees
     * exactly today's behavior. Gated on model_persisted for the same
     * reason coupling_diag_k_dc is just below: the estimate came from `m`,
     * the same fit that either did or didn't just get stored, and a ceiling
     * outliving the model it was derived from would be worse than neither.
     * Also requires predicted_max_ramp_c_per_hr > 0 -- pid_autotune_
     * estimate_max_ramp_c_per_hr() leaves it at the pre-run reset value
     * (0.0f, see autotune_begin_run_locked()) when it had nothing to
     * extrapolate from, and 0 is zone_cfg_t::max_ramp_c_per_hr's own
     * documented "never configured" encoding -- adopting it would silently
     * CLEAR an operator's existing ceiling rather than replace it with a
     * measurement. Logged, not propagated, on failure: same reasoning as
     * model_persisted's own failure just above -- the gains (and model) the
     * operator clicked Accept for are already live either way. */
    if (adopt_ceiling) {
        /* method is unconditionally AUTOTUNE_METHOD_STEP here: the RELAY
         * branch above always returns before reaching this point, so a
         * "method != AUTOTUNE_METHOD_STEP" check here would be dead code
         * (review finding) -- removed rather than kept as defence in depth,
         * since AUTOTUNE_CEILING_SKIPPED_RELAY_METHOD is already reported
         * from the one place RELAY actually reaches. */
        if (!model_persisted) {
            if (out != NULL) {
                out->adoption = AUTOTUNE_CEILING_SKIPPED_MODEL_NOT_PERSISTED;
            }
        } else if (predicted_max_ramp_ambient_c_per_hr <= 0.0f) {
            if (out != NULL) {
                out->adoption = AUTOTUNE_CEILING_SKIPPED_ZERO;
            }
        } else {
            /* Old value read fresh here (not cached earlier) so it reflects
             * whatever is actually stored right now, including a value an
             * operator set on /settings/zones after this run started. */
            float old_ceiling_c_per_hr = 0.0f;
            bool have_old = zones_config_get_max_ramp(zone, &old_ceiling_c_per_hr);
            if (out != NULL && have_old) {
                out->old_ceiling_c_per_hr = old_ceiling_c_per_hr;
            }
            if (!have_old) {
                /* Review fix: a failed read used to fall through to the
                 * final "nothing stored" branch below and adopt over it --
                 * indistinguishable from a zone that genuinely has no
                 * ceiling configured yet. Never adopt over a value we could
                 * not read. */
                if (out != NULL) {
                    out->adoption = AUTOTUNE_CEILING_SKIPPED_READ_FAILED;
                }
                ESP_LOGW(AT_TAG, "autotune zone %u: could not read the currently stored ramp ceiling -- "
                              "not adopting the predicted %.1f degC/hr ceiling over an unknown value",
                         zone, (double)predicted_max_ramp_ambient_c_per_hr);
            } else if (predicted_max_ramp_ambient_c_per_hr > ZONE_MAX_RAMP_C_PER_HR_MAX) {
                /* Out of range is a refusal, never a clamp -- adopting a
                 * clamped value would silently misrepresent what the fit
                 * actually predicted. Checked before the would-tighten
                 * comparison below so an out-of-range estimate is always
                 * reported as REJECTED_OUT_OF_RANGE, never masked by a
                 * stored ceiling that happens to already be tighter. */
                if (out != NULL) {
                    out->adoption = AUTOTUNE_CEILING_REJECTED_OUT_OF_RANGE;
                }
                ESP_LOGW(AT_TAG, "autotune zone %u: predicted ramp ceiling %.1f degC/hr exceeds max %.1f "
                              "degC/hr -- rejected, not clamped, max_ramp_c_per_hr left unchanged",
                         zone, (double)predicted_max_ramp_ambient_c_per_hr, (double)ZONE_MAX_RAMP_C_PER_HR_MAX);
            } else if (old_ceiling_c_per_hr > 0.0f &&
                predicted_max_ramp_ambient_c_per_hr <= old_ceiling_c_per_hr) {
                /* Never silently tighten: adopting a new estimate SMALLER
                 * than (or equal to) the stored, nonzero ceiling would make
                 * the zone's ramp ceiling more restrictive (or a no-op)
                 * without being asked -- skip instead of overwriting a
                 * looser (or equal) value. */
                if (out != NULL) {
                    out->adoption = AUTOTUNE_CEILING_SKIPPED_WOULD_TIGHTEN;
                }
                /* Review fix: this used to say the STORED ceiling was
                 * "already tighter or equal", backwards -- old_ceiling is
                 * the larger (looser) of the two here by construction
                 * (predicted < old_ceiling), so it is the NEW estimate that
                 * is tighter. The rule this guards is "never tighten an
                 * existing nonzero ceiling", so say that. */
                ESP_LOGI(AT_TAG, "autotune zone %u: predicted ramp ceiling %.1f degC/hr not adopted -- "
                              "it is tighter than the stored ceiling %.1f degC/hr, and adoption never "
                              "auto-tightens an existing nonzero ceiling",
                         zone, (double)predicted_max_ramp_ambient_c_per_hr, (double)old_ceiling_c_per_hr);
            } else if (!zones_config_set_max_ramp(zone, predicted_max_ramp_ambient_c_per_hr)) {
                /* Review fix: this failure used to be discarded entirely
                 * (the return value of zones_config_set_max_ramp() was
                 * ignored above and this branch didn't exist), so a rejected
                 * or failed persist looked identical to ADOPTED to every
                 * caller. Now reported distinctly. */
                if (out != NULL) {
                    out->adoption = AUTOTUNE_CEILING_FAILED_TO_PERSIST;
                    out->new_ceiling_c_per_hr = predicted_max_ramp_ambient_c_per_hr;
                }
                /* zones_config_set_max_ramp() writes the new ceiling into
                 * RAM and bumps s_config_generation BEFORE it calls
                 * nvs_save() -- so on an nvs_save() failure the new ceiling
                 * is already live in RAM for this boot, it just did not
                 * make it to flash. Say that, not "left unchanged". */
                ESP_LOGW(AT_TAG,
                         "autotune zone %u: gains and model accepted; predicted ramp ceiling (%.1f degC/hr) "
                         "is live in RAM for this boot but was not persisted -- will revert to %.1f degC/hr "
                         "on reboot",
                         zone, (double)predicted_max_ramp_ambient_c_per_hr, (double)old_ceiling_c_per_hr);
            } else {
                if (out != NULL) {
                    out->adoption = AUTOTUNE_CEILING_ADOPTED;
                    out->new_ceiling_c_per_hr = predicted_max_ramp_ambient_c_per_hr;
                }
                ESP_LOGI(AT_TAG, "autotune zone %u: adopted predicted ramp ceiling %.1f degC/hr (ambient-"
                              "evaluated) into max_ramp_c_per_hr",
                         zone, (double)predicted_max_ramp_ambient_c_per_hr);
            }
        }
    }
    /* PID_EXPANSION_PLAN.md section 3.2, "on-board identification pass" --
     * closes the last-named gap in that section: until now coupling_diag_k_dc
     * could only be set by hand or by a PC-side preset built from
     * coupled_ident.py's offline analysis, never by autotune. No new
     * identification path is built here -- see autotune_coupling_matrix_t's
     * own doc comment ("i == j is the direct/diagonal gain, identical to the
     * model in autotune_engine_status_t for the run that produced it"): the
     * DIRECT fit `m` this same accept() call is already writing to
     * model_k_dc via zones_config_set_model() above IS the diagonal cell,
     * because zone_index's own trace against its own duty step is exactly
     * what fills s_at.coupling.cell[zone_index][zone_index] in
     * autotune_finalize_fit() (see that function's TODO.md 6A.5(b) block). It is the
     * same rested single-zone step-excitation data the off-diagonal cross-
     * gain cells just above are fit from (same trace, same duty step, same
     * run) -- the "same kind of data" section 3.2 requires for this field to
     * mean the same thing as the matrix it is substituted into.
     *
     * Gated identically to model_persisted: this line is only reached once
     * the STEP-method settled/converged/tau_consistent gate above already
     * passed (or was explicitly overridden with ack_unsettled), the exact
     * same trustworthiness bar zones_config_set_model() and the cross-gain
     * persist in autotune_finalize_fit() both apply to this same fit. No separate
     * quality gate is invented here.
     *
     * Deliberately still just storage, same as the field's own doc comment
     * says: this pass populates coupling_diag_k_dc, it does not flip
     * `s_coupling_use_measured_diag_k_dc` in profile_executor_feedforward.c
     * -- that stays a separate, explicit decision, off by default, per this
     * task's constraints. Logged, not propagated, for the same reason
     * model_persisted's own failure isn't: the gains and model the operator
     * clicked Accept for are already live either way. */
    if (model_persisted) {
        bool diag_persisted = zones_config_set_coupling_diag_k_dc(zone, m.k_gain_c_per_duty);
        if (!diag_persisted) {
            ESP_LOGW(AT_TAG,
                     "autotune zone %u: gains and model accepted but coupling_diag_k_dc (%.4f degC/duty) was "
                     "rejected or failed to persist -- coupling solve stays on the ff_k_dc hybrid for this zone",
                     zone, (double)m.k_gain_c_per_duty);
        }
    }
    /* ZONES_CFG_VERSION 12->13: the tuning-quality record (set 1 -- see
     * zone_cfg_t::tuning_valid's own doc comment). Written ONLY after the
     * gains (above) and the model (just above) are both already persisted --
     * a quality record attached to a model that failed to save would
     * describe a fit the zone isn't actually running. zones_config_set_pid()
     * invalidated any PRIOR record when the gains changed beyond
     * zones_config_gain_changed()'s tolerance (see that function's own
     * comment), so this call is what re-establishes a fresh one for the run
     * that just completed. Edge: if the accepted gains equal the stored ones
     * within tolerance, set_pid leaves the old record valid, and a failure of
     * set_tuning_quality below then leaves that old record standing (it still
     * describes gains equal within tolerance to the live ones); a failure here is logged,
     * not propagated, for the exact same reason the model-persist failure
     * just above isn't -- the gains and model the operator actually clicked
     * Accept for are already live either way. */
    if (model_persisted) {
        zone_tuning_quality_t q = {
            .valid = true,
            .method = (uint8_t)method,
            .rule = (uint8_t)g.rule,
            .settled = m.settled,
            .extrapolation_converged = m.extrapolation_converged,
            .tau_consistent = m.tau_consistent_with_gain,
            .baseline_c = m.baseline_c,
            .step_ambient_c = step_ambient_c,
            .raw_rise_c = m.raw_rise_c,
            .rise_inf_c = m.rise_inf_c,
        };
        if (!zones_config_set_tuning_quality(zone, &q)) {
            ESP_LOGW(AT_TAG, "autotune zone %u: gains and model accepted but the tuning-quality record "
                          "was rejected or failed to persist", zone);
        }
    }

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    clear_block_if_any();
    s_at.state = AUTOTUNE_ENGINE_IDLE;
    xSemaphoreGive(s_at.lock);
    if (!m.settled) {
        ESP_LOGW(AT_TAG, "autotune zone %u: LOW-CONFIDENCE gains accepted and written to zone config "
                      "(ack_unsettled=true; fit never genuinely settled)",
                 zone);
    } else {
        ESP_LOGI(AT_TAG, "autotune zone %u: gains accepted and written to zone config", zone);
    }
    autotune_engine_release_zone_for_external_write(zone);
    return true;
}

