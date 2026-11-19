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
//   - ramp_assist_stretch_rate_c_per_s() IS the sec 7.2 control behaviour:
//     it reads the sustained-lagging zones' own achieved climb rate
//     (lag_held_s/lag_start_actual_c, already maintained above) and returns
//     the rate profile_executor.c's segment-stepping block should advance
//     target_c by INSTEAD of the commanded seg->ramp_c_per_hr, once ramp-
//     lock has held long enough (EXEC_SUSTAINED_LAG_S) to call the segment
//     unachievable at the commanded rate. It writes nothing itself -- the
//     caller is what actually assigns the returned rate to target_c's
//     advance, gated the same way seg->ramp_c_per_hr's ordinary advance
//     already was on `lock_ok`. The sentinel -1.0f (assist off, or no zone
//     both lagging and sustained) tells the caller to fall back to the
//     pre-existing freeze -- sec 7.1's lock, unconditionally, still
//     guarantees no zone is ever outrun regardless of whether this
//     function's stretch is what is currently moving target_c or the lock
//     alone is holding it still.
//   - ramp_assist_dwell_credit_tick() banks heat-work-weighted credit,
//     ALWAYS (same "report regardless of the flag" convention as the lag
//     functions above) -- it writes only zone_runtime_t.dwell_credit_s,
//     never target_c/segment_elapsed_s/dwelling, so dwell TIMING cannot be
//     affected by this function running or not.
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
    ramp_assist_zone_lag_tick_ex(z, lagging_now, dt_s, false);
}

/* EA C4 (docs/ELEMENT_AGING_PLAN.md F3): the ramp lock holds a zone at the
 * band edge, so sensor noise drops it out of "lagging" well inside 30 s and
 * lag_held_s reset every time. With stretch on, lag_held_s now resets only
 * after the zone has been continuously not lagging for EXEC_SUSTAINED_LAG_S;
 * shorter gaps freeze it. stretch_on false = the old reset-on-any-gap rule. */
void ramp_assist_zone_lag_tick_ex(zone_runtime_t *z, bool lagging_now, float dt_s, bool stretch_on)
{
    if (!lagging_now) {
        if (stretch_on && z->lag_held_s > 0.0f) {
            z->lag_clear_s += dt_s;
            if (z->lag_clear_s < EXEC_SUSTAINED_LAG_S) {
                return; /* flicker: keep lag_held_s and lag_sustained */
            }
        }
        z->lag_held_s = 0.0f;
        z->lag_sustained = false;
        z->lag_clear_s = 0.0f;
        return;
    }
    z->lag_clear_s = 0.0f;
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

// PID_EXPANSION_PLAN.md sec 7.2: auto-stretch, the actual control behaviour.
// Returns the rate (degC/s, always >= 0 -- direction is the caller's problem,
// same convention seg->ramp_c_per_hr already uses) the currently-lagging
// zones have ACTUALLY been achieving, for the caller to advance target_c by
// instead of the commanded seg->ramp_c_per_hr when ramp-lock (sec 7.1) would
// otherwise hold the setpoint indefinitely -- or the sentinel -1.0f when no
// stretch should happen (assist off, or no zone has lagged long enough to
// call the segment "unachievable" rather than "momentarily behind").
//
// Deliberately gates on lag_sustained (EXEC_SUSTAINED_LAG_S, ~30s), not the
// instantaneous lagging_mask bit alone: a brief hold during normal PID
// settling is not evidence the SCHEDULE is wrong, only that this tick is.
// Stretching on every momentary hold would make the setpoint's rate noisy
// tick to tick; waiting for "sustained" means auto-stretch only engages once
// ramp-lock has already proven it would otherwise hold indefinitely.
//
// The achieved rate is (actual_c - lag_start_actual_c) / lag_held_s -- the
// zone's own average climb since lag onset, exactly the arithmetic lag_
// start_actual_c was captured for (see its own doc comment,
// profile_executor_internal.h). Clamped to >= 0 so a zone that is
// momentarily falling (a load-cap window, a brief guard interaction) never
// pushes target_c backward -- the worst case degrades to "stretch adds zero
// distance this tick", not "the schedule runs in reverse".
//
// Takes the MINIMUM across every active, non-faulted, sustained-lagging
// zone -- the same conservative "never outrun the slowest one" direction
// sec 7.3's dwell-credit spend already uses (ramp_assist_dwell_credit_
// spend()) -- so a healthy zone's fast recent average can never make target_c
// outrun a genuinely-stuck neighbour sharing the same shared target_c.
//
// Never writes anything -- purely a read of already-updated zone_runtime_t
// state (lag_held_s/lag_start_actual_c/lag_sustained, all maintained by
// ramp_assist_zone_lag_tick() above, which the caller runs first each tick).
// This is also why the hard refusal (sec 7.2: a target above max_temp_c is
// never stretched) needs no check here: the caller clamps the resulting
// target_c to seg->target_c exactly as it always has (the "reached" logic in
// profile_executor.c's segment-stepping block is untouched by this
// function), and max_temp_c is already enforced against seg->target_c itself
// at firing start (profile_executor_run.c) -- there is no path through this
// function that could produce a target_c beyond what that start-time check
// already allowed.
float ramp_assist_stretch_rate_c_per_s(s_exec_state_t *ex, uint8_t lagging_mask, bool assist_enabled)
{
    if (!assist_enabled) {
        return -1.0f;
    }
    float min_rate_c_per_s = -1.0f; /* sentinel: "no sustained-lagging zone seen yet" */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &ex->zones[zi];
        if (!z->active || z->faulted) {
            continue;
        }
        if ((lagging_mask & (uint8_t)(1u << zi)) == 0) {
            continue;
        }
        if (!z->lag_sustained || z->lag_held_s <= 0.0f) {
            continue;
        }
        float rate_c_per_s = (z->actual_c - z->lag_start_actual_c) / z->lag_held_s;
        if (rate_c_per_s < 0.0f) {
            rate_c_per_s = 0.0f; /* never let a falling zone run the schedule backward */
        }
        if (min_rate_c_per_s < 0.0f || rate_c_per_s < min_rate_c_per_s) {
            min_rate_c_per_s = rate_c_per_s;
        }
    }
    return min_rate_c_per_s;
}


// PID_EXPANSION_PLAN.md sec 7.3: dwell credit accrual. Ports tools/PcTools/
// src/kilnctrl/ramp_assist.py's RampStep branch (~line 366-390) tick for
// tick:
//   in_band = band_bottom <= actual_c < target_c
//   if behind_schedule and in_band: credit += heat_work_weight(actual_c, target_c) * dt
// `segment_target_c` is the caller's seg->target_c (this ramp segment's
// final target), not the moving s_exec.target_c -- see this function's
// declaration comment (profile_executor_internal.h). ALWAYS runs regardless
// of ramp_assist_cfg_enabled(); the caller decides what to do with the
// accumulated total (ramp_assist_dwell_credit_spend(), below).
//
// DELIBERATELY NOT the ramp-lock's `lagging_now` (sec 7.1's `(target_c -
// actual_c) > EXEC_RAMP_LOCK_BAND_C`, 25C for every shipped config). That
// band and this credit gate answer two different questions and must stay
// two different booleans:
//   - ramp-lock's lagging_now: "is the zone SO far behind that the schedule
//     must freeze" -- correctly a wide band, and it must stay wide; it is
//     also what auto-stretch, the warning surfaces and the event log key
//     off of, and none of that changes here.
//   - this gate (`behind_schedule_now`): "is the zone behind AT ALL, however
//     slightly" -- deliberately narrow (zero), because the credit itself is
//     already scoped tight by `in_band` (half a cone step below
//     segment_target_c) and weighted by actual heat-work
//     (cone_table_heat_work_weight()). Tying it to the 25C lock band instead
//     made credit and in_band mutually exclusive: during a ramp, target_c <=
//     segment_target_c, so a lock-lagging zone is always >25C below
//     segment_target_c, while in_band requires being within half a cone
//     step -- under 25C everywhere in the Orton table except cone 019.
//     Measured result before this fix: credit was exactly zero at bisque,
//     cone 6 and cone 10, at every mass loading tested. See the commit this
//     function's history points at for the full measurement.
//
// If a future change makes this parameter track the ramp-lock band again
// (by reusing `lagging_now`, or by widening `behind_schedule_now`'s
// definition past "actual_c < target_c"), it will silently zero out dwell
// credit again exactly as before -- test_ramp_assist_credit_not_ramp_lock_
// gated in test_profile_executor_prestart.c exists to catch that.
void ramp_assist_dwell_credit_tick(zone_runtime_t *z, bool ramping_now, bool behind_schedule_now,
                                   float segment_target_c, float dt_s)
{
    if (!ramping_now || !behind_schedule_now) {
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
    /* REMOVED (2026-09-03, opus review of commit 5312e14, "DEFECT 2"): this
     * used to also fill a second "audit" accumulator via its own duplicate
     * cone_table_heat_work_weight() call. Deleted rather than kept -- see
     * zone_runtime_t's doc comment (profile_executor_internal.h, right
     * after dwell_credit_s) for the full justification. Real protection
     * against a wrong Ea/band/weight-function/dt now comes from
     * test_profile_executor_prestart.c's hand-computed weight pins. */
    float w;
    if (cone_table_heat_work_weight(z->actual_c, segment_target_c, &w) == CONE_TABLE_OK) {
        z->dwell_credit_s += w * dt_s;
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
        if (z->monitor_only) {
            continue; /* SPARE_RELAY_ONOFF_PLAN.md sec 10: never accrues credit, must not floor the minimum */
        }
        if (min_credit_s < 0.0f || z->dwell_credit_s < min_credit_s) {
            min_credit_s = z->dwell_credit_s;
        }
        /* "Spent once", unconditionally -- see this function's declaration
         * comment for why this reset happens even with assist_enabled ==
         * false (matches ramp_assist.py's z.credit_s = 0.0 outside its own
         * apply_dwell_credit branch). */
        z->dwell_credit_s = 0.0f;
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

// PID_EXPANSION_PLAN.md sec 7.6: bounded in-dwell dwell credit -- the read
// half. See these three functions' own declaration comments (profile_
// executor_internal.h, right after ramp_assist_dwell_credit_spend()) for the
// two-bound safety argument; this file only implements the arithmetic.

float ramp_assist_dwell_credit_peek_min_s(const s_exec_state_t *ex)
{
    float min_credit_s = -1.0f; /* sentinel: "no active zone seen yet" */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const zone_runtime_t *z = &ex->zones[zi];
        if (!z->active || z->faulted || z->monitor_only) {
            continue;
        }
        if (min_credit_s < 0.0f || z->dwell_credit_s < min_credit_s) {
            min_credit_s = z->dwell_credit_s;
        }
        /* Deliberately no reset here -- that is exactly what makes this a
         * "peek", safe to call every tick of a dwell. */
    }
    if (min_credit_s < 0.0f) {
        return 0.0f; /* no active zones */
    }
    return min_credit_s;
}

float ramp_assist_dwell_credit_total_spend_s(const s_exec_state_t *ex, float entry_applied_s,
                                             float cap_s)
{
    if (entry_applied_s < 0.0f) entry_applied_s = 0.0f;
    if (cap_s < 0.0f) cap_s = 0.0f;
    float live_topup_s = ramp_assist_dwell_credit_peek_min_s(ex);
    if (live_topup_s < 0.0f) live_topup_s = 0.0f;
    float total_s = entry_applied_s + live_topup_s;
    /* Bound 1: never more than cap_s, however large entry_applied_s or the
     * live top-up are individually or combined. */
    if (total_s > cap_s) {
        total_s = cap_s;
    }
    return total_s;
}

bool ramp_assist_dwell_target_reached(const s_exec_state_t *ex)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const zone_runtime_t *z = &ex->zones[zi];
        if (!z->active || z->faulted || z->monitor_only) {
            continue; /* a monitor-only zone cannot reach the heated target */
        }
        if (!z->actual_valid || z->actual_c < ex->target_c) {
            return false; /* Bound 2: not every active zone is there yet */
        }
    }
    return true; /* every active, non-faulted zone has reached target_c (or there are none) */
}

bool ramp_assist_dwell_too_hot(const s_exec_state_t *ex)
{
    return ex->dwelling && ex->dwell_arrived_hot && ramp_assist_zones_above_band(ex);
}

bool ramp_assist_zones_above_band(const s_exec_state_t *ex)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const zone_runtime_t *z = &ex->zones[zi];
        if (!z->active || z->faulted || z->monitor_only) {
            continue;
        }
        /* An invalid reading says nothing about direction; the ramp lock
         * already holds the schedule for it. */
        if (z->actual_valid && (z->actual_c - ex->target_c) > EXEC_RAMP_LOCK_BAND_C(zi)) {
            return true;
        }
    }
    return false;
}

/* ---- EA C3: arrival-gated hold (docs/ELEMENT_AGING_PLAN.md F2) ------------------------------ */

/* Stall detector, called every tick per active zone. No duty condition (a real cone-10 kiln runs
 * near 100% duty at the top even healthy): a zone is "stalled" when its signed rise over the last
 * EXEC_ARRIVAL_SETTLE_WINDOW_S, measured as a slope between the first and last 30 s averages of a
 * 10-bucket ring (not two single samples), is under EXEC_ARRIVAL_STALL_RATE_C_PER_MIN. Falling
 * counts as stalled. Needs a full window of valid readings; an invalid reading restarts it. The
 * caller restarts it (arrival_settle_reset) at dwell entry, segment change and pause/resume. */
void arrival_settle_reset(zone_runtime_t *z)
{
    z->settle_nb = 0;
    z->settle_acc = 0.0f;
    z->settle_acc_s = 0.0f;
    z->settle_stalled = false;
}

void arrival_settle_tick(zone_runtime_t *z, float actual_c, float duty, float dt_s)
{
    (void)duty; /* deliberately unused: no duty threshold (cone-10 kilns run ~100% healthy) */
    if (isnan(actual_c)) {
        arrival_settle_reset(z);
        return;
    }
    z->settle_acc += actual_c * dt_s;
    z->settle_acc_s += dt_s;
    if (z->settle_acc_s < EXEC_ARRIVAL_SETTLE_BUCKET_S) {
        return;
    }
    float avg = z->settle_acc / z->settle_acc_s;
    z->settle_acc = 0.0f;
    z->settle_acc_s = 0.0f;
    if (z->settle_nb >= EXEC_ARRIVAL_SETTLE_BUCKETS) {
        for (uint8_t i = 1; i < EXEC_ARRIVAL_SETTLE_BUCKETS; i++) z->settle_ring[i - 1] = z->settle_ring[i];
        z->settle_nb = EXEC_ARRIVAL_SETTLE_BUCKETS - 1;
    }
    z->settle_ring[z->settle_nb++] = avg;
    if (z->settle_nb >= EXEC_ARRIVAL_SETTLE_BUCKETS) {
        float span_min = (float)(EXEC_ARRIVAL_SETTLE_BUCKETS - 1) * EXEC_ARRIVAL_SETTLE_BUCKET_S / 60.0f;
        float rise = (z->settle_ring[EXEC_ARRIVAL_SETTLE_BUCKETS - 1] - z->settle_ring[0]) / span_min;
        z->settle_stalled = rise < EXEC_ARRIVAL_STALL_RATE_C_PER_MIN;
    } else {
        z->settle_stalled = false;
    }
}

/* One zone satisfies the arrival gate: at or above target - band (one-sided, overshoot never
 * blocks) or settled. An invalid (NaN) reading never satisfies. */
bool arrival_zone_ok(float actual_c, float target_c, float band_c, bool settled)
{
    if (isnan(actual_c)) {
        return false;
    }
    return (actual_c >= target_c - band_c) || settled;
}

/* A heating segment: ZONE_RAMP whose target is above the previous ZONE_RAMP segment's target (or
 * above run_start_c for the first). Cooling and flat segments are untouched by C3. */
bool arrival_segment_is_heating(const profile_segment_t *segs, uint8_t count, uint8_t index, float run_start_c)
{
    if (index >= count || segs[index].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
        return false;
    }
    float prior_c = run_start_c;
    for (int i = (int)index - 1; i >= 0; i--) {
        if (segs[i].seg_kind == PROFILE_SEG_KIND_ZONE_RAMP) {
            prior_c = segs[i].target_c;
            break;
        }
    }
    return segs[index].target_c > prior_c;
}

/* Lower bound of the planned duration: every dwell plus every rate-limited ramp. A step segment
 * (ramp_c_per_hr <= 0) has no known duration here and contributes 0 (documented in
 * docs/ELEMENT_AGING_PLAN.md: the budget is conservative), so the budget is never
 * larger than the dashboard's plan (the gate opens sooner, never later, than intended). Kept
 * local instead of calling profile_feasibility_plan_curve() so this module needs no extra link. */
float arrival_planned_lower_bound_s(const profile_segment_t *segs, uint8_t count, float start_c)
{
    float total_s = 0.0f;
    float prior_c = start_c;
    for (uint8_t i = 0; i < count; i++) {
        if (segs[i].seg_kind == PROFILE_SEG_KIND_ZONE_RAMP) {
            if (segs[i].ramp_c_per_hr > 0.0f) {
                total_s += fabsf(segs[i].target_c - prior_c) / segs[i].ramp_c_per_hr * 3600.0f;
            }
            total_s += (float)segs[i].dwell_min * 60.0f;
            prior_c = segs[i].target_c;
        } else if (segs[i].io_blocking) {
            total_s += (float)segs[i].dwell_min * 60.0f;
        }
    }
    return total_s;
}

float arrival_budget_s(float planned_total_s, uint16_t stretch_max_pct)
{
    return planned_total_s * ((float)stretch_max_pct / 100.0f);
}

/* True while the dwell timer must wait. Pure and stateless: the memory is arrival_wait_s (this
 * gate's own budget) and stretch_total_s (ramp-time stretch). The gate waits while arrival_wait_s
 * is under budget_s AND the two together are under the overall cap of 2 * budget_s. Stretch off, a
 * non-heating segment, or every zone arrived means no wait; an exhausted budget opens the gate. */
bool arrival_gate_waits(bool stretch_on, bool heating_seg, bool all_zones_ok, float arrival_wait_s,
                        float stretch_total_s, float budget_s)
{
    if (!stretch_on || !heating_seg || all_zones_ok) {
        return false;
    }
    return arrival_wait_s < budget_s && (arrival_wait_s + stretch_total_s) < 2.0f * budget_s;
}

/* The whole C3 decision for one dwelling tick (stretch on, heating segment): returns true while the
 * dwell timer must wait. Waiting spends the stretch budget (arrival_wait_s and the per-segment
 * slot, but arrival_wait_s, not stretch_total_s). When the budget is exhausted and a zone has still not arrived, the gate opens and the
 * largest shortfall over the driving zones is recorded in ex->target_shortfall_c. The caller
 * undoes this tick's timer accumulation when this returns true. */
bool arrival_gate_tick(s_exec_state_t *ex, bool stretch_on, uint16_t stretch_max_pct, float dt_s)
{
    if (!stretch_on || !arrival_segment_is_heating(ex->profile.segments, ex->profile.segment_count,
                                                   ex->segment_index, ex->run_start_c)) {
        return false;
    }
    if (ex->arrival_gate_seg != (int16_t)ex->segment_index) {
        /* Dwell entry (or first tick after a pause/resume/run start): restart every stall window. */
        ex->arrival_gate_seg = (int16_t)ex->segment_index;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) arrival_settle_reset(&ex->zones[zi]);
    }
    float target_c = ex->profile.segments[ex->segment_index].target_c;
    bool all_ok = true;
    float shortfall_c = 0.0f;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const zone_runtime_t *z = &ex->zones[zi];
        if (!z->active || z->faulted || !profile_executor_zone_drives_run(zi)) continue;
        float band_c = (z->guard_cfg.progress_band_c > 0.0f) ? z->guard_cfg.progress_band_c : 3.0f;
        if (!arrival_zone_ok(z->actual_c, target_c, band_c, z->settle_stalled)) {
            all_ok = false;
        }
        /* Shortfall counts every driving zone outside the band, whether the gate opens by stall or
         * by budget exhaustion. */
        if (!isnan(z->actual_c) && z->actual_c < target_c - band_c) {
            float miss_c = target_c - z->actual_c;
            if (miss_c > shortfall_c) shortfall_c = miss_c;
        }
    }
    float budget_s = arrival_budget_s(
        arrival_planned_lower_bound_s(ex->profile.segments, ex->profile.segment_count, ex->run_start_c),
        stretch_max_pct);
    bool wait = arrival_gate_waits(true, true, all_ok, ex->arrival_wait_s, ex->stretch_total_s, budget_s);
    if (wait) {
        if (ex->segment_index < PROFILE_MAX_SEGMENTS) {
            ex->stretch_by_segment_s[ex->segment_index] += dt_s;
        }
        ex->arrival_wait_s += dt_s;
    } else if (shortfall_c > ex->target_shortfall_c) {
        ex->target_shortfall_c = shortfall_c;
    }
    return wait;
}

/* The dwell-done decision, factored so the gate's effect is testable: while the arrival gate
 * waits, neither the nominal exit nor the credited early exit may fire (credit is not spent). */
bool exec_dwell_done(bool too_hot, bool arrival_wait, uint32_t elapsed_s, uint32_t nominal_s,
                     uint32_t credited_threshold_s, bool target_reached)
{
    return !too_hot && !arrival_wait &&
           ((elapsed_s >= nominal_s) || (elapsed_s >= credited_threshold_s && target_reached));
}
