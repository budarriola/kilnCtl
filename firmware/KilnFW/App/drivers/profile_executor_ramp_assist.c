// profile_executor_ramp_assist -- PID_EXPANSION_PLAN.md sec 7.1/7.2/7.4:
// sustained-lag detection and auto-stretch INSTRUMENTATION for the ramp-lock
// machinery that already lives in profile_executor.c (sec 7.1, "already
// built, do not rebuild").
//
// HONEST ACCOUNTING (see the task report this file's commit message points
// at): neither function in this file adds new schedule-altering control
// behaviour.
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
//     dwell -- sec 7.3's dwell credit is explicitly out of this file's
//     scope and lands separately). It never changes target_c or the
//     segment's ramp rate either. Gated on ramp_assist_cfg_enabled(): with
//     the flag off, the lock still runs exactly as it always has (7.1's
//     guarantee is and remains unconditional), this file just does not
//     count that time as "assist".
//
// HARD REFUSAL (sec 7.2's "a target above the kiln's permitted maximum is a
// hard refusal, never stretched"): enforced entirely by profile_executor_
// run.c's existing max_temp_c re-check at firing start, not by anything
// here. Because neither function below ever writes target_c/seg->target_c,
// there is no code path in this file that COULD push the commanded target
// past what that start-time refusal already allowed through -- the
// constraint holds by construction, not by an added runtime check.
#include "profile_executor_internal.h"

#include <string.h>

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
