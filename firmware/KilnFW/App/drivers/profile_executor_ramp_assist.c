// profile_executor_ramp_assist -- PID_EXPANSION_PLAN.md sec 7.1/7.2/7.3/7.4:
// sustained-lag detection, auto-stretch INSTRUMENTATION, and sec 7.3's dwell
// credit, all built on top of the ramp-lock machinery that already lives in
// profile_executor.c (sec 7.1, "already built, do not rebuild").
//
// HONEST ACCOUNTING (see the task report this file's commit message points
// at): the lag/stretch functions add no new schedule-altering control
// behaviour; the two dwell-credit functions at the bottom of this file DO --
// see each function's own doc comment for exactly which half is gated.
//   - ramp_assist_zone_lag_tick() only WATCHES the existing ramp-lock
//     signal (s_exec.ramp_lock_lagging_mask, computed in profile_executor.c
//     from EXEC_RAMP_LOCK_BAND_C) for how long it has held CONTINUOUSLY per
//     zone, and turns that into a reportable "lag_sustained" condition. It
//     never touches target_c, segment_elapsed_s, or any other schedule
//     state -- the lock itself, unconditionally, is what keeps every ramp
//     endpoint reachable (sec 7.1). This function runs regardless of
//     ramp_assist_cfg_enabled(): the owner asked for raw lag visibility
//     during PID testing even with assist off.
//   - ramp_assist_stretch_tick() only RECORDS, in seconds, how long the
//     existing lock has held the setpoint during an actual ramp (not a
//     dwell -- sec 7.3's dwell credit is a separate accumulator, below).
//     It never changes target_c or the segment's ramp rate either. Gated on
//     ramp_assist_cfg_enabled(): with the flag off, the lock still runs
//     exactly as it always has (7.1's guarantee is and remains
//     unconditional), this file just does not count that time as "assist".
//   - ramp_assist_dwell_credit_tick() banks heat-work-weighted credit,
//     ALWAYS (same "report regardless of the flag" convention as the lag
//     functions above) -- it writes only zone_runtime_t.dwell_credit_s/
//     dwell_credit_audit_s, never target_c/segment_elapsed_s/dwelling, so
//     dwell TIMING cannot be affected by this function running or not.
//   - ramp_assist_dwell_credit_spend() is the one function in this file
//     whose RETURN VALUE is gated on ramp_assist_cfg_enabled() -- see its
//     own doc comment (profile_executor_internal.h) for exactly what stays
//     unconditional (the credit reset) vs. what is gated (the nonzero
//     spend). Its caller (profile_executor.c's dwelling-transition code)
//     is what actually turns that return value into a shortened dwell.
//
// HARD REFUSAL (sec 7.2's "a target above the kiln's permitted maximum is a
// hard refusal, never stretched"): enforced entirely by profile_executor_
// run.c's existing max_temp_c re-check at firing start, not by anything
// here. Because none of the functions below ever writes target_c/seg->
// target_c, there is no code path in this file that COULD push the
// commanded target past what that start-time refusal already allowed
// through -- the constraint holds by construction, not by an added runtime
// check.
#include "profile_executor_internal.h"

#include <math.h>
#include <string.h>

#include "cone_table.h"

void ramp_assist_zone_lag_tick(zone_runtime_t *z, bool lagging_now, float dt_s)
{
    if (!lagging_now) {
        z->lag_held_s = 0.0f;
        z->lag_sustained = false;
        return;
    }
    if (z->lag_held_s <= 0.0f) {
        /* Rising edge -- snapshot the temperature this lag started from, so
         * a caller reporting an achieved rate later has a baseline. actual_c
         * is already this tick's fresh reading (profile_executor.c's
         * reading loop runs before the lock/lag update), so this is the
         * temperature AT the first lagging tick, not one tick stale. */
        z->lag_start_actual_c = z->actual_c;
    }
    z->lag_held_s += dt_s;
    z->lag_sustained = (z->lag_held_s >= EXEC_SUSTAINED_LAG_S);
}

void ramp_assist_stretch_tick(s_exec_state_t *ex, uint8_t segment_index, bool assist_enabled,
                              bool ramping_now, bool lock_held_now, float dt_s)
{
    if (!assist_enabled || !ramping_now || !lock_held_now) {
        return;
    }
    if (segment_index < PROFILE_MAX_SEGMENTS) {
        ex->stretch_by_segment_s[segment_index] += dt_s;
    }
    ex->stretch_total_s += dt_s;
}

// PID_EXPANSION_PLAN.md sec 7.3: dwell credit accrual. Ports tools/PcTools/
// src/kilnctrl/ramp_assist.py's RampStep branch (~line 366-390) tick for
// tick:
//   in_band = band_bottom <= actual_c < target_c
//   if lagging and in_band: credit += heat_work_weight(actual_c, target_c) * dt
// `segment_target_c` is the caller's seg->target_c (this ramp segment's
// final target), not the moving s_exec.target_c -- see this function's
// declaration comment (profile_executor_internal.h). ALWAYS runs regardless
// of ramp_assist_cfg_enabled(); the caller decides what to do with the
// accumulated total (ramp_assist_dwell_credit_spend(), below).
void ramp_assist_dwell_credit_tick(zone_runtime_t *z, bool ramping_now, bool lagging_now,
                                   float segment_target_c, float dt_s)
{
    if (!ramping_now || !lagging_now) {
        return;
    }
    float band_bottom_c;
    if (cone_table_band_bottom_c(segment_target_c, &band_bottom_c) != CONE_TABLE_OK) {
        /* Out of the cone table's covered range -- cone_table reports this
         * explicitly (CONE_TABLE_ERR_OUT_OF_RANGE_LOW/HIGH), so the correct
         * behaviour is "no credit, no crash", never a silent/garbage band. */
        return;
    }
    bool in_band = (z->actual_c >= band_bottom_c) && (z->actual_c < segment_target_c);
    if (!in_band) {
        return;
    }
    float w;
    if (cone_table_heat_work_weight(z->actual_c, segment_target_c, &w) == CONE_TABLE_OK) {
        z->dwell_credit_s += w * dt_s;
    }
    /* Independent audit accumulator -- its own separate cone_table call, its
     * own separate `+=`, deliberately not derived from the line above (or
     * from `w`) so a regression confined to the real accrual does not also
     * corrupt this reference value. See this field's doc comment
     * (profile_executor_internal.h) and PID_EXPANSION_PLAN.md sec 7.3's
     * "DEFECT 1" writeup for why that independence is the whole point. */
    float w_audit;
    if (cone_table_heat_work_weight(z->actual_c, segment_target_c, &w_audit) == CONE_TABLE_OK) {
        z->dwell_credit_audit_s += w_audit * dt_s;
    }
}

// PID_EXPANSION_PLAN.md sec 7.3: dwell credit spend, called exactly once per
// dwell entry (profile_executor.c's dwelling-transition code, both the
// normal ramp-reaches-target case and the ramp_c_per_hr<=0 instant-jump
// case -- see that call site's own comment). Mirrors ramp_assist.py's
// DwellStep branch (~line 424-432):
//   spend = z.credit_s if apply_dwell_credit else 0.0
//   spend = min(spend, nominal_s)
//   z.dwell_remaining_s = nominal_s - spend
//   z.credit_s = 0.0
// with one firmware-specific adaptation: this executor has ONE shared
// segment_elapsed_s/dwelling pair across every active zone (unlike ramp_
// assist.py's independent per-zone dwell timers), so the single spend
// value applied to that shared timer is the MINIMUM of every active,
// non-faulted zone's own banked credit -- never any zone's alone. That is
// the conservative direction (matches cone_table.h's own documented
// under-credit-is-safe stance): no zone is ever credited for heat work it
// did not itself accrue, at the cost of a heavily-lagging zone's slower
// neighbour capping everyone's payback.
float ramp_assist_dwell_credit_spend(s_exec_state_t *ex, float nominal_dwell_s, bool assist_enabled)
{
    float min_credit_s = -1.0f; /* sentinel: "no active zone seen yet" */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &ex->zones[zi];
        if (!z->active || z->faulted) {
            continue;
        }
        if (min_credit_s < 0.0f || z->dwell_credit_s < min_credit_s) {
            min_credit_s = z->dwell_credit_s;
        }
        /* "Spent once", unconditionally -- see this function's declaration
         * comment for why this reset happens even with assist_enabled ==
         * false (matches ramp_assist.py's z.credit_s = 0.0 outside its own
         * apply_dwell_credit branch). */
        z->dwell_credit_s = 0.0f;
        z->dwell_credit_audit_s = 0.0f;
    }
    if (min_credit_s < 0.0f) {
        return 0.0f; /* no active zones -- nothing to spend */
    }
    if (!assist_enabled) {
        return 0.0f; /* the one gated half of dwell credit -- see ramp_assist_cfg.h */
    }
    float spend = min_credit_s;
    if (spend < 0.0f) spend = 0.0f;               /* never a negative dwell */
    if (nominal_dwell_s < 0.0f) nominal_dwell_s = 0.0f;
    if (spend > nominal_dwell_s) spend = nominal_dwell_s; /* clamp to the nominal dwell */
    return spend;
}
