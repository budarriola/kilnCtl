/* Relay/IO segment machinery and guard escalation -- split out of
 * profile_executor.c (2026-09-01, "files over 1500 lines should be broken
 * up where it makes sense"). See profile_executor_internal.h's own doc
 * comment for the full multi-way split this is one piece of. Covers: the
 * relay-authority-gated zone relay writer, the TODO relay/IO segment
 * runtime (io_seg_.../io_segs_...), guard trip escalation
 * (escalate_guard_trip()/guard9's stale-tick fault), and the unowned-relay
 * sweep -- all of it the
 * "who is allowed to command a contact, and how a trip or a segment forces
 * one off" half of the executor, as opposed to the control-loop math or the
 * task/lifecycle machinery that call into it. */

#include "profile_executor_internal.h"

#include <math.h>

#include "esp_log.h"

#include "autotune_engine.h"
#include "heat_enable.h"
#include "kiln_io_owner.h"
#include "relay_authority.h"
#include "sim_backend.h"
#include "zones_http.h"

void apply_relay(uint8_t zi, bool want_on)
{
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(zi, &mask) || mask == 0) {
        s_exec.zones[zi].relay_commanded_on = false;
        return;
    }

    /* Claimed the moment this run can name the mask at all, in BOTH
     * directions and before the authority gate -- not only when a relay is
     * actually energized. A commanded-off write that silently failed leaves a
     * coil closed just as effectively as a commanded-on one, and by the time
     * the sweep is looking for strays it has no way to reconstruct which
     * masks this run once addressed. Over-claiming costs nothing (the sweep
     * intersects with what kiln_io believes is still closed); under-claiming
     * is the whole failure this exists to catch. */
    s_exec.claimed_relay_mask |= mask;

    /* Evaluated EVERY tick now, unconditionally -- NOT only when want_on is
     * true, which is what this function did until a review caught the
     * regression it caused (2026-09-XX PWM/progress-window fix, defect 1).
     * z->heat_blocked is the single source of truth profile_executor_
     * guard_commanded_duty() reads to decide whether the guards see zero or
     * the intended duty; if it were only refreshed on ticks that wanted
     * heat, a duty commanded partway through a PWM window (want_on=true on
     * the on-pulse, false on the off-pulse) would leave heat_blocked stale
     * on every off-pulse -- true from the last on-pulse's real evaluation,
     * but READ by the guard-duty helper as "this tick is genuinely
     * blocked", so a zone with a real, contiguous block ends up reporting
     * ALTERNATING zero/nonzero to the guards at the PWM period instead of a
     * genuine contiguous zero. That broke guard 3 (welded relay) during an
     * authority block specifically -- guard 3 needs a contiguous duty<=0
     * run, which it had before this whole fix (apply_relay() always forced
     * relay_commanded_on=false while blocked) and lost when heat_blocked's
     * staleness was introduced. Evaluating unconditionally restores that: a
     * genuinely blocked zone now reads heat_blocked=true on EVERY tick,
     * want_on or not, so profile_executor_guard_commanded_duty() sees a
     * true contiguous zero again. */
    uint32_t sources = 0;
    bool blocked = relay_authority_zone_blocked(s_exec.safety, zi, &sources);
    bool edge = (blocked != s_exec.zones[zi].heat_blocked) ||
                (blocked && sources != s_exec.zones[zi].heat_blocked_sources);
    if (edge) {
        if (blocked && want_on) {
            /* The common, most-actionable case: this tick actually wanted
             * heat and got refused. */
            ESP_LOGW(PE_TAG, "zone %u WANTS HEAT BUT IS BLOCKED: sources 0x%02X -- no relay will "
                          "close and the run will otherwise look normal",
                     zi, (unsigned)sources);
        } else if (blocked) {
            /* Block state changed on a tick that did not itself want heat
             * (e.g. a PWM off-pulse, or a zone between demands) -- still
             * worth a line, just without claiming this exact tick wanted
             * heat. */
            ESP_LOGW(PE_TAG, "zone %u's heat is blocked: sources 0x%02X", zi, (unsigned)sources);
        } else {
            ESP_LOGI(PE_TAG, "zone %u heat no longer blocked", zi);
        }
    }
    s_exec.zones[zi].heat_blocked = blocked;
    s_exec.zones[zi].heat_blocked_sources = blocked ? sources : 0u;
    if (want_on && blocked) {
        want_on = false;
    }

    if (s_exec.io) {
        /* AUTHORIZED, not the manual gate -- see kiln_io_owner.h's top
         * comment. relay_authority_zone_blocked() just above already
         * applied this run's own gate; kiln_io_owner just serializes the
         * actual write against uart_bridge.c/dashboard_set_relay() (2026-08-19,
         * TODO.md 10.14 Phase 1). */
        esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(mask, want_on ? mask : 0);
        if (err != ESP_OK) {
            ESP_LOGW(PE_TAG, "kiln_io_owner_command_set_relay_mask_authorized failed: %s -- relay "
                          "state for zone %u is unknown",
                     esp_err_to_name(err), zi);
        }
    }
    /* No-op unless CONFIG_KILNCTL_SIM_PLANT. Fed the POST-gate decision, not
     * what control wanted, so the simulated kiln heats only when a real one
     * would have. */
    sim_backend_note_zone_relay(zi, want_on);
    s_exec.zones[zi].relay_commanded_on = want_on;
}

/* What guards 1/2/3/7 (thermal_guard.c) should be told this zone's
 * commanded_duty is, for THIS tick -- factored out of the apply-relays-and-
 * guards loop below purely so it is directly unit-testable (same reasoning
 * as this file's other small pure-decision helpers). See that loop's own
 * comment, right above where this is called, for the full defect this
 * replaces and the load-cap reasoning behind the choice below.
 *
 * REVISED (review defect 1): the first version of this function took an
 * additional want_relay_on_this_tick parameter and only zeroed intended_duty
 * when (want_relay_on_this_tick && heat_blocked). That gate was WRONG, and a
 * regression against the pre-fix code: want_relay_on_this_tick is itself the
 * POST-PWM instantaneous relay decision (heater_output_duty()'s return, see
 * the caller), so under a genuine authority block at a partial intended
 * duty it alternated true/false at the PWM period right along with the
 * relay -- true on an on-pulse (heat_blocked freshly true that tick,
 * correctly zeroing), false on an off-pulse (the gate itself false, so the
 * function returned intended_duty UNZEROED even though the zone was, in
 * fact, still blocked that whole time). Guard 3 (welded relay) needs a
 * CONTIGUOUS duty<=0 run to arm; this alternation broke it during exactly
 * the scenario guard 3 exists for -- a welded/shorted contact discovered
 * while heat is authority-blocked. The pre-fix code never had this bug
 * (apply_relay() always forced relay_commanded_on=false while blocked, a
 * genuine contiguous zero) -- the bug was introduced by this function's own
 * first version, not inherited.
 *
 * Root cause: heat_blocked is now refreshed by apply_relay() on EVERY tick
 * (see that function's own comment on this exact change), so it is no
 * longer only "fresh when want_on was true" -- there is nothing left for a
 * want_relay_on_this_tick gate to usefully condition, and gating on it was
 * actively wrong whenever heat_blocked correctly stayed true across a
 * PWM off-pulse. Dropped entirely: this function now trusts heat_blocked
 * unconditionally, which is correct precisely because the caller keeps it
 * unconditionally fresh. */
float profile_executor_guard_commanded_duty(bool heat_blocked, float intended_duty)
{
    return heat_blocked ? 0.0f : intended_duty;
}

/* Guard 1's own effective rate requirement for THIS tick -- factored out of
 * the apply-relays-and-guards loop for the same reason profile_executor_
 * guard_commanded_duty() was (direct unit-testability), see that loop's own
 * comment on review defect 2 for the full "a fixed absolute rate has no
 * knowledge of the commanded ramp" reasoning.
 *
 * configured_rate_c_per_min is the zone's own guard_cfg.sanity_rate_c_per_min
 * (0 meaning "not configured" -- substituted with thermal_guard.c's own
 * 0.5 C/min default here so the min() below always compares two real
 * numbers, mirroring effective_f()'s convention in that file without
 * needing to export it). target_rate_c_per_s is s_exec.target_rate_c_per_s,
 * exactly 0.0f during a dwell/step-segment/ramp-lock tick (see that field's
 * own comment) -- deliberately left AT the configured rate in that case
 * (returned unchanged), not capped to zero, so a lagging DWELL still has to
 * catch up at the zone's full configured rate; the cap only ever narrows
 * the requirement while a ramp is genuinely still moving. */
/* THIS ZONE's own commanded setpoint rate, for feeding into profile_executor_
 * guard_sanity_rate() above -- the fix for a real gap opened by the per-zone
 * approach-rate cap (ZONES_CFG_VERSION 17->18, d800a60, PER_ZONE_TARGET_
 * DESIGN_STUDY.md option (b)).
 *
 * That pass made thermal_guard_input_t.setpoint_c per-zone (zone_commanded_
 * setpoint_c(), i.e. z->effective_target_c when this zone has a cap) but left
 * guard 1's paired rate requirement keyed off the SHARED s_exec.target_rate_
 * c_per_s. The two must describe the same setpoint: guard 1's whole reason
 * for capping the expected rise at the commanded ramp rate (see profile_
 * executor_guard_sanity_rate()'s own comment, and test_profile_executor_
 * prestart.c's test_healthy_ramp_lag_does_not_false_trip_guard1) is "never
 * demand that a zone outrun the setpoint it is chasing." A zone capped at
 * 20 C/hr under a segment ramping at 100 C/hr is asked to climb 0.33 C/min
 * while guard 1, reading the shared 1.67 C/min, would fall back to the bare
 * 0.5 C/min default and demand half again more rise than the zone's OWN
 * setpoint moves -- exactly the false trip that helper exists to prevent,
 * and precisely the numbers test_healthy_ramp_lag_still_trips_without_the_
 * rate_cap() already proves do trip.
 *
 * cap_c_per_hr is this zone's zone_cfg_t::approach_rate_cap_c_per_hr, 0 =
 * uncapped: the shared rate is returned VERBATIM in that case, so every zone
 * that has never set a cap (the default, and every zone before the field
 * existed) is bit-identical to before this function existed.
 *
 * still_approaching is (z->effective_target_c != s_exec.target_c) -- a capped
 * zone whose own setpoint has not yet arrived at the shared destination is
 * genuinely still moving at the cap, INCLUDING while the shared schedule has
 * already reached its target and started dwelling (target_rate_c_per_s is
 * 0.0f then). Reporting 0 there would hand profile_executor_guard_sanity_
 * rate() the dwell case and demand the full configured rate from a zone whose
 * setpoint is, in fact, still climbing at the cap. Once the capped zone HAS
 * arrived, the shared rate governs again (min() with the cap, since a cap can
 * only ever tighten) and a stationary shared setpoint correctly yields 0 --
 * the "a lagging dwell must still catch up at the full configured rate"
 * behaviour, unchanged. */
float profile_executor_guard_zone_ramp_rate(float shared_rate_c_per_s, float cap_c_per_hr,
                                            bool still_approaching)
{
    if (!(cap_c_per_hr > 0.0f)) {
        return shared_rate_c_per_s; /* uncapped -- verbatim, bit-identical */
    }
    float cap_c_per_s = cap_c_per_hr / 3600.0f;
    if (still_approaching) {
        return cap_c_per_s;
    }
    float shared_abs = fabsf(shared_rate_c_per_s);
    return (shared_abs < cap_c_per_s) ? shared_abs : cap_c_per_s;
}

float profile_executor_guard_sanity_rate(float configured_rate_c_per_min, float target_rate_c_per_s)
{
    float configured = (configured_rate_c_per_min > 0.0f) ? configured_rate_c_per_min : 0.5f;
    float ramp_rate_c_per_min = fabsf(target_rate_c_per_s) * 60.0f;
    if (ramp_rate_c_per_min > 0.0f && ramp_rate_c_per_min < configured) {
        return ramp_rate_c_per_min;
    }
    return configured;
}

/* Must be called with s_exec.lock held. */
void force_zone_relay_off(uint8_t zi)
{
    heater_output_force_off(&s_exec.zones[zi].heater_state);
    apply_relay(zi, false);
    s_exec.zones[zi].duty = 0.0f;
}

/* Must be called with s_exec.lock held. */
void force_all_relays_off(void)
{
    if (s_exec.state == PROFILE_EXEC_IDLE) {
        return; /* nothing loaded -- nothing to turn off */
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (s_exec.zones[zi].active) {
            force_zone_relay_off(zi);
        }
    }
}

/* Hands every relay this run ever claimed (claimed_relay_mask, see
 * apply_relay()'s comment) back to RELAY_OWNER_NONE. profile_executor_halt()
 * already did this on its own exit path; this is the SAME release, called
 * from every OTHER path that leaves RUNNING/PAUSED for a state that is not
 * "still an in-progress run" -- normal completion (DONE), a guard trip
 * (FAULTED, all three escalate_guard_trip() branches that set it), and the
 * watchdog's own forced FAULTED transition. Before this existed, only
 * halt() released the claim, so a run that finished on its own or faulted
 * kept every relay it touched tagged RELAY_OWNER_PROFILE until an operator
 * explicitly dismissed it -- confirmed on the bench: a guard-1 fault ended a
 * firing and relay 1 was still refused to /api/relay, the LCD Temperature
 * page and the UART bridge as "owned by a running profile" with no profile
 * running. relay_authority_release_mask() is a plain overwrite (see
 * relay_authority.c), so calling this and then having halt() call it again
 * later (an operator dismissing the same FAULTED/DONE run) is harmless --
 * releasing an already-released mask changes nothing.
 *
 * Deliberately NOT called from profile_executor_pause(): a paused run is
 * still a run in progress by TODO.md section 0's own reasoning (it hands the
 * claim to RELAY_OWNER_MANUAL instead of releasing it) -- releasing here
 * would let a manual command fight a firing that is one profile_executor_
 * resume() away from driving those same relays again. Must be called with
 * s_exec.lock held. */
void release_profile_relay_claim(void)
{
    relay_authority_release_mask(s_exec.claimed_relay_mask);
    /* The shared heat claim (relay_authority.h) taken atomically right
     * before this run's s_exec.state was set to RUNNING -- see
     * profile_executor_run()'s own comment at that call site. Safe to call
     * unconditionally: relay_authority_heat_zone_claim_end() is a no-op if
     * this run never actually held it (refused before reaching that point).
     * Every path that leaves RUNNING/PAUSED for good funnels through this
     * function except profile_executor_halt(), which releases it directly
     * alongside its own relay_authority_release_mask() call for the same
     * reason it doesn't call this whole function (see halt()'s comment). */
    relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
    /* ...and give K4 back too. Every terminal transition out of RUNNING/
     * PAUSED except profile_executor_halt() (which does it inline, alongside
     * its own duplicate of the two calls above, for the reason its comment
     * gives) funnels through here -- normal completion, all three
     * escalate_guard_trip() branches, and the watchdog's forced FAULTED
     * transition -- so this is the single release point for the firing's
     * heat-enable request. Called AFTER force_all_relays_off()/
     * force_zone_relay_off() at every one of those sites: dropping the zone
     * relay is never gated on, or delayed by, giving K4 back
     * (heat_enable.h's ordering rule). Idempotent, so calling it on a path
     * that never acquired costs nothing. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
}

/* ---- TODO relay/IO segments (owner's request, profiles_http.h's
 * profile_seg_kind_t doc comment) ---------------------------------------
 *
 * These four functions are the executor-side half of the feature: applying
 * a segment's command, and force-releasing it on every path that leaves
 * RUNNING. All four must be called with s_exec.lock held, same as every
 * other s_exec-touching static in this file. */

/* Second, independent zone-ownership check (the storage-side one is
 * profiles_http.c's profile_relay_is_zone_owned(), run at save time) -- a
 * relay can be reassigned to a zone AFTER a profile was saved, same
 * reload-time hazard zones_http.c's relay_mask comment and rules_task.c's
 * compute_heater_relay_mask() both already document for the rule engine,
 * and the exact reason the ramp-ceiling feasibility check just above this
 * function's call site is ALSO re-run at start rather than trusted from
 * save time. relay_1_4 is 1-based. */
bool relay_io_target_is_zone_owned(uint8_t relay_1_4, uint8_t *out_zone_index)
{
    uint8_t bit = (uint8_t)(1u << (relay_1_4 - 1u));
    uint8_t zone_count = zones_config_get_thermo_count();
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (zones_config_get_relay_mask(zi, &zone_mask) && (zone_mask & bit) != 0) {
            if (out_zone_index) *out_zone_index = zi;
            return true;
        }
    }
    return false;
}

/* Applies segment `idx`'s command once and starts tracking it. Called the
 * first tick segment_index reaches a RELAY_IO segment (see the
 * segment-stepping block) -- never re-applied on later ticks while the same
 * segment is still current, so a manual override of a NON-BLOCKING segment's
 * relay in between is possible but is also exactly what relay_authority's
 * ownership claim below exists to prevent for the blocking/relay case. */
void io_seg_start(uint8_t idx, const profile_segment_t *seg)
{
    io_seg_runtime_t *r = &s_exec.io_segs[idx];
    memset(r, 0, sizeof(*r));
    r->active = true;
    r->is_relay = (seg->io_target >= PROFILE_IO_TARGET_RELAY_BASE) &&
                  (seg->io_target < PROFILE_IO_TARGET_RELAY_BASE + KILN_IO_RELAY_COUNT);
    r->target = seg->io_target;
    r->blocking = seg->io_blocking != 0;
    r->state_on = seg->io_state != 0;
    r->leave_on_at_end = seg->io_leave_on_at_end != 0;
    r->remaining_s = (float)(seg->dwell_min * 60u);

    if (r->is_relay) {
        uint8_t bit = (uint8_t)(1u << (r->target - PROFILE_IO_TARGET_RELAY_BASE));
        /* Same claim-before-write discipline apply_relay() uses: claimed the
         * moment this run can name the bit, in both directions, so the sweep
         * below can always account for it even if the write itself fails. */
        s_exec.claimed_relay_mask |= bit;
        relay_authority_claim_mask(bit, RELAY_OWNER_PROFILE);
        if (s_exec.io) {
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(bit, r->state_on ? bit : 0);
            if (err != ESP_OK) {
                ESP_LOGW(PE_TAG, "relay/IO segment %u: relay %u write failed: %s -- state is unknown",
                         idx + 1, r->target, esp_err_to_name(err));
            }
        }
    } else if (s_exec.io) {
        esp_err_t err = kiln_io_owner_command_set_io(r->target - PROFILE_IO_TARGET_IO_BASE + 1u, r->state_on);
        if (err != ESP_OK) {
            ESP_LOGW(PE_TAG, "relay/IO segment %u: IO_%u write failed: %s -- state is unknown",
                     idx + 1, r->target - PROFILE_IO_TARGET_IO_BASE + 1u, esp_err_to_name(err));
        }
    }
    ESP_LOGI(PE_TAG, "relay/IO segment %u: %s %u %s (%s, %lus hold)", idx + 1,
             r->is_relay ? "relay" : "IO_", r->is_relay ? r->target : (uint8_t)(r->target - PROFILE_IO_TARGET_IO_BASE + 1u),
             r->state_on ? "ON" : "OFF", r->blocking ? "blocking" : "non-blocking",
             (unsigned long)seg->dwell_min * 60u);
}

/* Ends segment `idx`'s command -- either commanding it off, or (only when
 * honor_leave_on is true AND the segment itself asked for it via
 * leave_on_at_end) leaving it exactly as last commanded and handing
 * ownership back to NONE so it becomes an ordinary, manually-reachable
 * relay/IO from this point on, same as if an operator had always owned it.
 *
 * honor_leave_on is true ONLY on the clean DONE path (see the
 * segment-stepping block and force_all_relays_off()'s caller in the main
 * tick loop). It is deliberately FALSE on every other path that can call
 * this -- profile_executor_halt(), a global or per-zone guard trip
 * escalating to FAULTED, and the guard-9/watchdog stale-tick and safety-trip
 * force-offs -- because those are all abnormal-stop paths where the safe
 * default (relay actually goes off) must win over a per-segment convenience
 * preference, regardless of what the segment asked for. Only a clean,
 * intentional "the schedule finished exactly as planned" end honors the
 * owner's flag; every other ending is treated the same as the flag's own
 * default (off). A natural mid-run timeout (io_segs_tick() below) also
 * always passes false: the segment finished on its own, which is not "the
 * profile ended while it was still running" at all. */
void io_seg_finish(uint8_t idx, bool honor_leave_on)
{
    io_seg_runtime_t *r = &s_exec.io_segs[idx];
    if (!r->active) {
        return;
    }
    bool leave_on = honor_leave_on && r->leave_on_at_end && r->state_on;

    if (r->is_relay) {
        uint8_t bit = (uint8_t)(1u << (r->target - PROFILE_IO_TARGET_RELAY_BASE));
        if (!leave_on && s_exec.io) {
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(bit, 0);
            if (err != ESP_OK) {
                ESP_LOGE(PE_TAG, "relay/IO segment %u: force-off of relay %u failed: %s -- sweep_unowned_relays() "
                              "will keep retrying",
                         idx + 1, r->target, esp_err_to_name(err));
            }
        }
        /* Either way this run is done naming this bit: on the off path
         * nothing more needs it; on the leave-on path an unowned energized
         * relay is intentional (the owner's explicit opt-in) and must NOT be
         * reported by sweep_unowned_relays() as a stray -- see that
         * function's own doc comment on claimed_relay_mask. Releasing the
         * relay_authority claim in both cases means the relay is reachable
         * from /api/relay and the UART bridge again either way, exactly as
         * if no profile had ever touched it. */
        s_exec.claimed_relay_mask &= (uint8_t)~bit;
        relay_authority_release_mask(bit);
    } else if (!leave_on && s_exec.io) {
        esp_err_t err = kiln_io_owner_command_set_io(r->target - PROFILE_IO_TARGET_IO_BASE + 1u, false);
        if (err != ESP_OK) {
            ESP_LOGE(PE_TAG, "relay/IO segment %u: force-off of IO_%u failed: %s", idx + 1,
                     r->target - PROFILE_IO_TARGET_IO_BASE + 1u, esp_err_to_name(err));
        }
    }
    if (leave_on) {
        ESP_LOGW(PE_TAG, "relay/IO segment %u left ON at run end (leave_on_at_end) -- now unowned, reachable "
                      "manually",
                 idx + 1);
    }
    r->active = false;
}

/* Sweeps every segment this run has ever started -- the DONE/FAULTED/HALT/
 * stale-tick backstop, analogous to force_all_relays_off() for zone relays.
 * Safe to call every tick regardless of state: io_seg_finish() is a no-op
 * for a segment that is already inactive, so repeated calls (e.g. every tick
 * of a PAUSED or FAULTED run, or every tick after DONE) cost nothing once
 * the sweep has actually finished. */
void io_segs_force_all_off(bool honor_leave_on)
{
    for (uint8_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        io_seg_finish(i, honor_leave_on);
    }
}

/* Ticks every ACTIVE, NON-BLOCKING segment's own hold timer, independent of
 * which ramp/dwell segment segment_index currently points at -- this is what
 * "runs alongside the next segment" actually means at 1Hz: the timer keeps
 * counting down no matter how many other segments the shared schedule moves
 * through while it does. A natural (in-run) expiry always force-offs
 * (honor_leave_on=false) -- see io_seg_finish()'s doc comment for why that is
 * correct and not merely the safe default. Must be called once per RUNNING
 * tick, with s_exec.lock held. */
void io_segs_tick(float dt_s)
{
    for (uint8_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        io_seg_runtime_t *r = &s_exec.io_segs[i];
        if (!r->active || r->blocking) {
            continue; /* a blocking segment is finished by the segment-stepping block itself */
        }
        r->remaining_s -= dt_s;
        if (r->remaining_s <= 0.0f) {
            io_seg_finish(i, false);
        }
    }
}

/* Escalation policy (TODO.md 6A.6, "decide which -- see 6A.6"): guards whose
 * failure mode is severe/board-wide (a welded relay, an out-of-range
 * reading, an electrically faulted sensor) assert the GLOBAL fault source,
 * which blocks every zone via relay_authority_on_blocked() and faults the
 * WHOLE run, not just the tripping zone. Guards whose failure mode is
 * specific to one zone's physics only block that zone, via
 * relay_authority_zone_blocked()'s per-zone mask -- the run continues for
 * any other still-healthy active zone UNLESS
 * zones_config_get_continue_on_zone_trip() is false (the default,
 * TODO.md 6A.3's "abort the whole firing" policy) -- in that case a
 * per-zone trip also faults every other active zone, just without
 * asserting the board-wide safety-link fault the `global` branch does.
 * Returns true if this trip faulted the whole run (global trip, abort
 * policy, or the last active zone just faulted anyway), false if the run
 * continues. Must be called with s_exec.lock held. */
bool escalate_guard_trip(uint8_t zi, thermal_guard_trip_t reason, const char *detail)
{
    bool global = (reason == THERMAL_GUARD_TRIP_RUNAWAY || reason == THERMAL_GUARD_TRIP_MAX_TEMP ||
                  reason == THERMAL_GUARD_TRIP_MIN_TEMP || reason == THERMAL_GUARD_TRIP_SENSOR_INVALID);
    uint32_t source = (reason == THERMAL_GUARD_TRIP_SENSOR_INVALID) ? SAFETY_FAULT_SRC_THERMO
                                                                    : SAFETY_FAULT_SRC_THERMAL_SANITY;

    if (global) {
        if (s_exec.safety) {
            esp_err_t err = safety_link_set_fault_source(s_exec.safety, source, true);
            if (err != ESP_OK) {
                ESP_LOGE(PE_TAG, "safety_link_set_fault_source(0x%02X) failed: %s", (unsigned)source,
                         esp_err_to_name(err));
            }
        }
        s_exec.global_fault_source = source;
        for (uint8_t zi2 = 0; zi2 < MAX31856_CHANNEL_COUNT; zi2++) {
            if (!s_exec.zones[zi2].active) continue;
            s_exec.zones[zi2].faulted = true;
            strncpy(s_exec.zones[zi2].fault_reason, detail, sizeof(s_exec.zones[zi2].fault_reason) - 1);
            s_exec.zones[zi2].fault_reason[sizeof(s_exec.zones[zi2].fault_reason) - 1] = '\0';
            s_exec.zones[zi2].fault_guard = reason;
            force_zone_relay_off(zi2);
        }
        s_exec.state = PROFILE_EXEC_FAULTED;
        strncpy(s_exec.fault_reason, detail, sizeof(s_exec.fault_reason) - 1);
        s_exec.fault_reason[sizeof(s_exec.fault_reason) - 1] = '\0';
        s_exec.fault_guard = reason;
        /* Abnormal stop: force off regardless of any segment's
         * leave_on_at_end -- see io_seg_finish()'s doc comment for why a
         * guard trip never honors it. */
        io_segs_force_all_off(false);
        release_profile_relay_claim();
        ESP_LOGE(PE_TAG, "GLOBAL thermal guard tripped on zone %u, whole run faulted: %s", zi, detail);
        return true;
    }

    relay_authority_set_zone_blocked(zi, true);
    s_exec.zones[zi].per_zone_blocked = true;
    s_exec.zones[zi].faulted = true;
    strncpy(s_exec.zones[zi].fault_reason, detail, sizeof(s_exec.zones[zi].fault_reason) - 1);
    s_exec.zones[zi].fault_reason[sizeof(s_exec.zones[zi].fault_reason) - 1] = '\0';
    s_exec.zones[zi].fault_guard = reason;
    force_zone_relay_off(zi);
    ESP_LOGE(PE_TAG, "zone %u thermal guard tripped (per-zone): %s", zi, detail);

    /* TODO.md 6A.3's "default policy on a single-zone trip: abort the whole
     * firing" -- the remaining zones would keep dumping heat into a chamber
     * whose temperature is now partly unmeasured, and the ware is already
     * ruined; "continue" is the option that needs justifying, so it is the
     * one that requires an explicit opt-in
     * (zones_config_get_continue_on_zone_trip()). This does NOT assert the
     * board-wide SAFETY_FAULT_SRC_* bit the `global` branch above does --
     * the trip's cause is this zone's physics specifically, not a hardware
     * condition threatening every zone, so only the executor's run is
     * faulted, not the safety link. */
    if (!zones_config_get_continue_on_zone_trip()) {
        for (uint8_t zi2 = 0; zi2 < MAX31856_CHANNEL_COUNT; zi2++) {
            if (!s_exec.zones[zi2].active || s_exec.zones[zi2].faulted) continue;
            s_exec.zones[zi2].faulted = true;
            strncpy(s_exec.zones[zi2].fault_reason, detail, sizeof(s_exec.zones[zi2].fault_reason) - 1);
            s_exec.zones[zi2].fault_reason[sizeof(s_exec.zones[zi2].fault_reason) - 1] = '\0';
            s_exec.zones[zi2].fault_guard = reason;
            force_zone_relay_off(zi2);
        }
        s_exec.state = PROFILE_EXEC_FAULTED;
        snprintf(s_exec.fault_reason, sizeof(s_exec.fault_reason),
                "zone %u thermal guard tripped, whole firing aborted per policy: %s", zi, detail);
        s_exec.fault_guard = reason;
        io_segs_force_all_off(false); /* abnormal stop -- see the GLOBAL branch above */
        release_profile_relay_claim();
        ESP_LOGE(PE_TAG, "zone %u per-zone trip abandoned the whole firing (continue_on_zone_trip is off)", zi);
        return true;
    }

    bool all_faulted = true;
    for (uint8_t zi2 = 0; zi2 < MAX31856_CHANNEL_COUNT; zi2++) {
        if (s_exec.zones[zi2].active && !s_exec.zones[zi2].faulted) {
            all_faulted = false;
            break;
        }
    }
    if (all_faulted) {
        s_exec.state = PROFILE_EXEC_FAULTED;
        snprintf(s_exec.fault_reason, sizeof(s_exec.fault_reason), "every active zone individually faulted; last: %s",
                detail);
        s_exec.fault_guard = reason;
        io_segs_force_all_off(false); /* abnormal stop -- see the GLOBAL branch above */
        release_profile_relay_claim();
        ESP_LOGE(PE_TAG, "every active zone faulted -- whole run faulted");
        return true;
    }
    return false;
}

/* Guard 9's own fault-assertion step, factored out of watchdog_task_entry()'s
 * for(;;) loop body so a host test can call it directly -- the loop itself
 * cannot be, same "no seam without restructuring the module" limit as
 * executor_task_entry() (see test_profile_executor_prestart.c's relay-claim
 * test block comment), but this one function has no such limit: it is a
 * plain static function taking s_exec.lock as a precondition, exactly like
 * escalate_guard_trip() above.
 *
 * Audit 2026-08-27 item 2: this used to only call safety_link_set_fault_
 * source(..., true) and stop there -- nothing ever deasserted it, so a
 * single stale control-task tick left SAFETY_FAULT_SRC_APP latched
 * board-wide until reboot, blocking every relay-ON everywhere (including any
 * later, different fault, since relay_authority_on_blocked() only reports
 * "blocked", not which bit) -- the same class of bug escalate_guard_trip()'s
 * global branch already avoids via global_fault_source/clear_this_runs_
 * faults(). OR'd in, not assigned: an earlier global guard trip may already
 * be sitting in global_fault_source, and clear_this_runs_faults() clears the
 * whole mask in one safety_link_set_fault_source() call -- overwriting here
 * would silently drop that other source from ever being cleared.
 *
 * Must be called with s_exec.lock held. */
void guard9_assert_stale_tick_fault(void)
{
    if (s_exec.safety) {
        safety_link_set_fault_source(s_exec.safety, SAFETY_FAULT_SRC_APP, true);
    }
    s_exec.global_fault_source |= SAFETY_FAULT_SRC_APP;
}

/* Must be called with s_exec.lock held. */
void clear_this_runs_faults(void)
{
    if (s_exec.global_fault_source != 0 && s_exec.safety) {
        esp_err_t err = safety_link_set_fault_source(s_exec.safety, s_exec.global_fault_source, false);
        if (err != ESP_OK) {
            ESP_LOGE(PE_TAG, "clearing fault source 0x%02X failed: %s", (unsigned)s_exec.global_fault_source,
                     esp_err_to_name(err));
        }
    }
    s_exec.global_fault_source = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active) continue;
        if (s_exec.zones[zi].per_zone_blocked) {
            relay_authority_set_zone_blocked(zi, false);
            s_exec.zones[zi].per_zone_blocked = false;
        }
        thermal_guard_clear(&s_exec.zones[zi].guard_state);
        s_exec.zones[zi].faulted = false;
    }
}


/* ---- config reload while running (TODO.md 6A.7) ------------------------------ */

/* De-energizes a specific mask instead of "whatever mask this zone owns now",
 * which is all apply_relay() can express -- it re-reads the live config every
 * call. The one case that needs the distinction is an operator re-assigning
 * relays mid-firing: the contacts that must open are the ones the zone owned
 * a moment ago, and by the time the reload notices, the config can no longer
 * name them. Skipping this would leave those relays latched closed under a
 * mask no zone controls any more -- nothing would ever command them off
 * again, not even a guard trip or halt(), since every one of those paths also
 * goes through the live mask. Must be called with s_exec.lock held. */
void force_relay_mask_off(uint8_t zi, uint8_t mask)
{
    heater_output_force_off(&s_exec.zones[zi].heater_state);
    s_exec.claimed_relay_mask |= mask; /* see apply_relay() -- this is the one caller that can be handed a mask the live config no longer knows */
    if (s_exec.io && mask != 0) {
        /* AUTHORIZED -- same reasoning as apply_relay() above. */
        esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(mask, 0);
        if (err != ESP_OK) {
            ESP_LOGE(PE_TAG, "zone %u: dropping superseded relay mask 0x%02X failed: %s -- "
                          "those contacts may still be closed and nothing owns them now",
                     zi, mask, esp_err_to_name(err));
        }
    }
    sim_backend_note_zone_relay(zi, false);
    s_exec.zones[zi].relay_commanded_on = false;
    s_exec.zones[zi].duty = 0.0f;
}

/* The backstop for every path above (TODO.md 6A.7's "unowned-relay sweep").
 *
 * Everything else in this file that can OPEN a relay -- apply_relay(),
 * force_zone_relay_off(), halt/pause, a guard trip, force_relay_mask_off() --
 * can only address contacts it can still *name*, and all but the last name
 * them through the live zone config. So there is a family of failures with no
 * recovery path at all: one kiln_io_set_relay_mask() that returns an error and
 * leaves a coil energized, or a mask edit whose force-off didn't land, and
 * those contacts are then closed under a mask no zone owns any more. Nothing
 * downstream ever looks at them again -- a later guard trip re-opens the NEW
 * mask, halt() re-opens the NEW mask, and the kiln keeps heating from a relay
 * the firmware has forgotten it commanded. Guard 9 does call
 * kiln_io_all_relays_off(), but only once the control task has stopped
 * ticking, which is a different fault entirely: here the task is alive and
 * confidently driving the wrong set of contacts.
 *
 * The three masks this intersects, and why each one is needed:
 *
 *   kiln_io_get_relay_shadow()   what the board believes is still CLOSED.
 *       kiln_io.c refuses to update this from a failed write and re-syncs it
 *       from the part instead (see kiln_io_set_relay_mask()), so it is honest
 *       about exactly the failure this function exists for -- and it is what
 *       keeps the common case free: once a stray is actually cleared the
 *       shadow drops the bit, so the steady state is a compare and no I2C
 *       traffic at all, which is what makes this affordable at 1 Hz.
 *
 *   claimed_relay_mask           what THIS run has ever commanded.
 *       Without it this would be "open every relay no zone currently owns",
 *       and relays 1-4 are not the executor's exclusive property: the
 *       dashboard's manual /api/relay and the UART bridge's SET_RELAY can
 *       both energize any relay during a firing, gated only by
 *       relay_authority_on_blocked(), with no notion of who else is driving.
 *       An operator holding a damper or a blower on through a manual relay
 *       would have it chattered off once a second by a "safety" feature. A
 *       relay this run never touched is not this run's to open.
 *
 *   ~owned                       who may legitimately hold one right now.
 *       Active zones of this run whatever their fault state -- a faulted
 *       zone's relays are still that zone's to command off, and its mask is
 *       still resolvable, so it is not stranded. Plus any zone autotune is
 *       running on: autotune_engine.c drives relays through its own
 *       apply_relay() and the two engines are only mutually exclusive
 *       PER ZONE (autotune_engine_run() refuses a zone this run is driving,
 *       profile_executor_run() refuses a zone autotune holds), so a step test
 *       on zone 2 alongside a firing on zones 0-1 is a supported combination
 *       and its contacts must survive this.
 *
 * A zone whose live mask can no longer be READ contributes nothing to owned,
 * which is deliberate and is the second half of reload_zone_config()'s
 * dropped-zone branch: that branch force-opens the cached mask, and if that
 * write failed, this is what keeps retrying it.
 *
 * Reaching a non-zero stray mask at all means an earlier force-off failed or
 * an edit stranded contacts, so it is an ERROR every time it happens rather
 * than once -- the log repeating at 1 Hz is proportionate to a coil that is
 * still closed and still refusing to open. Must be called with s_exec.lock
 * held; the autotune query below takes s_at.lock while we hold s_exec.lock,
 * which is safe only because autotune never does the reverse (its one call
 * into this module, profile_executor_zone_is_active(), is made before it
 * takes s_at.lock). */
void sweep_unowned_relays(void)
{
    if (!s_exec.io) {
        return;
    }

    uint8_t owned = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active && !autotune_engine_is_active_on_zone(zi)) {
            continue;
        }
        uint8_t mask = 0;
        if (zones_config_get_relay_mask(zi, &mask)) {
            owned |= mask;
        }
    }

    uint8_t stray = (uint8_t)(kiln_io_get_relay_shadow(s_exec.io) & s_exec.claimed_relay_mask & (uint8_t)~owned);
    if (stray == 0) {
        return;
    }

    ESP_LOGE(PE_TAG, "UNOWNED RELAY(S) 0x%02X still closed (TODO.md 6A.7 sweep): claimed 0x%02X by this run, "
                  "owned 0x%02X by an active zone or an autotune run -- an earlier force-off failed or a "
                  "mask edit stranded them; forcing off",
             stray, s_exec.claimed_relay_mask, owned);
    /* AUTHORIZED -- same reasoning as apply_relay() above. */
    esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(stray, 0);
    if (err != ESP_OK) {
        ESP_LOGE(PE_TAG, "unowned-relay sweep could not open 0x%02X: %s -- those contacts are still closed and "
                      "the expander is not answering",
                 stray, esp_err_to_name(err));
    }
}

/* Re-reads zone zi's settings and folds them into the live run. Returns true
 * if anything actually moved (the caller only uses this for the summary
 * line). Called only from reload_config_if_changed(), with s_exec.lock held
 * and after this tick's readings have been stored, so the bumpless seed below
 * uses a measurement from this tick rather than the previous one.
 *
 * What is deliberately NOT touched here:
 *   - thermal_guard_state_t. A latched trip must survive a config edit, or
 *     "edit a threshold" becomes an undocumented way to clear a fault and
 *     re-energize a kiln that just tripped. TODO.md 6A.3 makes halt() the one
 *     explicit acknowledgement path, and a reload is not an acknowledgement.
 *   - heater_output_state_t (except where a mask/mode change forces the zone
 *     off outright). Resetting it would discard the min-on/min-off timers and
 *     the current window's accumulated on-time, i.e. an operator nudging
 *     window_ms could machine-gun a mechanical contactor -- the exact wear
 *     TODO.md 6A.1's min-on/min-off exists to prevent. The in-flight window
 *     finishes on the old timing; the next one uses the new. */
