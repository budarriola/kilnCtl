#include "profile_feasibility.h"

#include <math.h>

#include "MAX31856.h"
#include "zones_http.h"

/* Ambient reference for the plant model. profile_executor.c captures a real
 * one from the MAX31856 cold junction at firing start, but this module is
 * asked its question at *edit* time -- on a web page, possibly with the kiln
 * cold and the thermocouples unread -- so it uses the same constant
 * profile_executor.c falls back to (FALLBACK_AMBIENT_C, 20 C). The verdict is
 * insensitive to a few degrees of error here: every kiln temperature that
 * matters is hundreds of degrees above ambient, so a 5 C mis-estimate moves
 * the computed headroom by well under 1%.
 *
 * Not read from the live cold junction on purpose: that value drifts upward
 * as the board warms, and a feasibility colour that changed while the user
 * was looking at it, with no edit having been made, would be worse than a
 * slightly stale one. */
#define FEASIBILITY_AMBIENT_C 20.0f

/* A rate within this fraction of the modelled maximum is still called OK.
 * The maximum is the *asymptotic open-loop* rate at full power; a real closed
 * loop cannot sit at u = 1.0 indefinitely and still regulate, and the fitted
 * K/tau themselves carry fit error. Demanding 100% of the model's ceiling
 * would flag schedules that a real kiln comfortably runs. 0.90 leaves a 10%
 * margin, which is narrower than the 20%-of-ceiling *warning* band
 * profiles_http.c already applies to the user-entered ceiling -- these are
 * different questions and deliberately have different margins. */
#define FEASIBILITY_RATE_MARGIN 0.90f

/* Steady-state ceiling margin: a target within this many degrees of
 * T_amb + K is called UNREACHABLE. Sitting exactly at the asymptote requires
 * u = 1.0 forever and infinite time to arrive; a target that close is not
 * something the kiln reaches in a firing. */
#define FEASIBILITY_CEILING_MARGIN_C 5.0f

const char *profile_feasibility_verdict_str(profile_seg_verdict_t v)
{
    switch (v) {
    case PROFILE_SEG_OK:          return "ok";
    case PROFILE_SEG_TOO_FAST:    return "too_fast";
    case PROFILE_SEG_UNREACHABLE: return "unreachable";
    case PROFILE_SEG_UNKNOWN:
    default:                      return "unknown";
    }
}

/* Ordering used by the roll-ups: bigger is worse. See the header for why
 * UNKNOWN sits above OK but below the two real failures. */
static int verdict_rank(profile_seg_verdict_t v)
{
    switch (v) {
    case PROFILE_SEG_OK:          return 0;
    case PROFILE_SEG_UNKNOWN:     return 1;
    case PROFILE_SEG_TOO_FAST:    return 2;
    case PROFILE_SEG_UNREACHABLE: return 3;
    default:                      return 1;
    }
}

static profile_seg_verdict_t worse(profile_seg_verdict_t a, profile_seg_verdict_t b)
{
    return verdict_rank(b) > verdict_rank(a) ? b : a;
}

profile_seg_verdict_t profile_feasibility_segment(uint8_t zone_index, float start_c,
                                                  const profile_segment_t *seg)
{
    if (!seg) {
        return PROFILE_SEG_UNKNOWN;
    }

    float k_dc = 0.0f, tau_s = 0.0f, dead_time_s = 0.0f;
    if (!zones_config_get_model(zone_index, &k_dc, &tau_s, &dead_time_s)) {
        return PROFILE_SEG_UNKNOWN; /* getter could not answer at all */
    }
    /* zones_http.c documents all-zeros as the "no model identified" encoding,
     * and warns explicitly that a caller treating it as a real model divides
     * by zero. This is that caller, and this is the check. Anything
     * non-finite or non-positive is treated the same way -- unanswerable, not
     * infeasible. */
    if (!isfinite(k_dc) || !isfinite(tau_s) || k_dc <= 0.0f || tau_s <= 0.0f) {
        return PROFILE_SEG_UNKNOWN;
    }
    (void)dead_time_s;
    /* dead_time_s is deliberately unused here rather than silently dropped:
     * transport delay shifts WHEN the kiln responds, it does not change the
     * sustained rate it can hold once it is responding. It costs a fixed
     * offset at the start of a ramp (and matters a great deal to the PID
     * tuning), but it cannot make an achievable ramp rate unachievable, which
     * is the only question this module answers. */

    const float t_amb = FEASIBILITY_AMBIENT_C;
    const float ceiling_c = t_amb + k_dc; /* steady state at u = 1: T - T_amb = K */

    float target = seg->target_c;
    if (!isfinite(target) || !isfinite(start_c)) {
        return PROFILE_SEG_UNKNOWN;
    }

    /* Steady-state ceiling. Checked before the rate, and reported as its own
     * verdict, because it is the more serious failure: TOO_FAST means "slow
     * this segment down and it works", UNREACHABLE means the kiln will sit
     * below the target at full power until the operator gives up. */
    if (target > ceiling_c - FEASIBILITY_CEILING_MARGIN_C) {
        return PROFILE_SEG_UNREACHABLE;
    }

    float rate = seg->ramp_c_per_hr;
    if (!isfinite(rate) || rate <= 0.0f) {
        /* rate <= 0 is "no rate limit -- go as fast as the kiln can"
         * (profile_executor.c jumps the setpoint straight to target). There
         * is no commanded rate to be too fast for, so once the target itself
         * is reachable the segment is feasible by construction. */
        return PROFILE_SEG_OK;
    }

    /* Policy ceiling first: the user-entered max_ramp_c_per_hr that
     * profiles_http.c enforces at save and profile_executor.c re-checks at
     * start. Kept here so one verdict per segment covers both the policy and
     * the physics; 0 means "never configured", which the existing code
     * already treats as "every nonzero rate is over the ceiling". */
    float max_ramp = 0.0f;
    if (zones_config_get_max_ramp(zone_index, &max_ramp) && rate > max_ramp) {
        return PROFILE_SEG_TOO_FAST;
    }

    /* Physics. First-order plant: dT/dt = (K*u - (T - T_amb)) / tau.
     *
     *   heating, u = 1:  dT/dt = (K - (T - T_amb)) / tau      [C/s]
     *   cooling, u = 0:  dT/dt = -(T - T_amb) / tau, magnitude (T - T_amb)/tau
     *
     * An electric kiln has no active cooling, so the u = 0 expression is the
     * whole of its cooling ability -- a commanded cool faster than natural
     * loss is precisely the case that has to go red, and it is the one an
     * operator is most likely to write without realising.
     *
     * WORST POINT of the segment: both expressions are monotone in T, in
     * opposite directions. Heating headroom (K - (T - T_amb)) SHRINKS as T
     * rises, so a heating segment is hardest at its TOP end -- the target.
     * Natural cooling rate (T - T_amb) also shrinks as T falls, so a cooling
     * segment is hardest at its BOTTOM end -- again the target. Evaluating at
     * the target is therefore the worst case for both directions, and
     * evaluating anywhere else (the midpoint, the start) would pass segments
     * that stall before they finish. */
    float worst_t = target;
    bool heating = (target >= start_c);

    float max_rate_c_per_s;
    if (heating) {
        float headroom = k_dc - (worst_t - t_amb);
        if (headroom <= 0.0f) {
            return PROFILE_SEG_UNREACHABLE; /* belt-and-braces; the ceiling test above caught this */
        }
        max_rate_c_per_s = headroom / tau_s;
    } else {
        float above_ambient = worst_t - t_amb;
        if (above_ambient <= 0.0f) {
            /* Target at or below ambient: the kiln cannot be driven below the
             * room, and its cooling rate goes to zero as it approaches it. A
             * commanded rate down there cannot be met. */
            return PROFILE_SEG_TOO_FAST;
        }
        max_rate_c_per_s = above_ambient / tau_s;
    }

    float max_rate_c_per_hr = max_rate_c_per_s * 3600.0f;
    if (rate > FEASIBILITY_RATE_MARGIN * max_rate_c_per_hr) {
        return PROFILE_SEG_TOO_FAST;
    }
    return PROFILE_SEG_OK;
}

int64_t profile_feasibility_plan_curve(const profile_segment_t *segments, uint8_t segment_count,
                                       float start_c, profile_plan_point_t *out_points,
                                       size_t out_cap, size_t *out_point_count)
{
    if (out_point_count) {
        *out_point_count = 0;
    }
    if (!segments || segment_count == 0) {
        return -1;
    }

    float t = 0.0f;
    float cur_c = start_c;
    bool unknown = false;
    size_t n = 0;

#define ADD_POINT(tt, cc)                       \
    do {                                          \
        if (out_points && n < out_cap) {           \
            out_points[n].t = (tt);                 \
            out_points[n].c = (cc);                 \
        }                                            \
        n++;                                          \
    } while (0)

    ADD_POINT(t, cur_c);
    for (uint8_t i = 0; i < segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        const profile_segment_t *seg = &segments[i];
        float target = seg->target_c;

        if (seg->ramp_c_per_hr > 0.0f) {
            float dist = fabsf(target - cur_c);
            t += dist / seg->ramp_c_per_hr * 3600.0f;
        } else {
            /* No rate constraint -- the setpoint jumps, but the kiln's
             * actual arrival time is unknowable here. Plotted as a
             * zero-width step; the true verdict is the -1 returned below. */
            unknown = true;
        }
        cur_c = target;
        ADD_POINT(t, cur_c);

        uint32_t dwell_s = seg->dwell_min * 60u;
        if (dwell_s > 0) {
            t += (float)dwell_s;
            ADD_POINT(t, cur_c);
        }
    }
#undef ADD_POINT

    if (out_point_count) {
        *out_point_count = n;
    }
    return unknown ? -1 : (int64_t)(t + 0.5f);
}

profile_seg_verdict_t profile_feasibility_profile(uint8_t zone_index, const profile_t *p,
                                                  profile_seg_verdict_t *out_segments,
                                                  size_t out_cap)
{
    if (!p || p->segment_count == 0) {
        return PROFILE_SEG_UNKNOWN;
    }

    /* The run starts from the room, then each segment starts where the
     * previous one's target left it -- the same walk profile_executor.c does
     * with real setpoints. */
    float start_c = FEASIBILITY_AMBIENT_C;
    profile_seg_verdict_t rollup = PROFILE_SEG_OK;

    for (uint8_t i = 0; i < p->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        profile_seg_verdict_t v = profile_feasibility_segment(zone_index, start_c, &p->segments[i]);
        if (out_segments && i < out_cap) {
            out_segments[i] = v;
        }
        rollup = worse(rollup, v);
        start_c = p->segments[i].target_c;
    }
    return rollup;
}

profile_seg_verdict_t profile_feasibility_profile_mask(uint8_t zone_mask, const profile_t *p,
                                                       profile_seg_verdict_t *out_segments,
                                                       size_t out_cap)
{
    if (!p || p->segment_count == 0) {
        return PROFILE_SEG_UNKNOWN;
    }

    uint8_t mask = zone_mask;
    if (mask == 0) {
        /* Zone-agnostic (a builtin catalogue entry): judge it against every
         * configured zone, since that is the set it could be run on. */
        uint8_t n = zones_config_get_thermo_count();
        mask = (n >= 8) ? 0xFFu : (uint8_t)((1u << n) - 1u);
    }
    if (mask == 0) {
        return PROFILE_SEG_UNKNOWN; /* no zones configured -- nothing to judge against */
    }

    if (out_segments) {
        for (size_t i = 0; i < out_cap; i++) {
            out_segments[i] = PROFILE_SEG_OK;
        }
    }

    profile_seg_verdict_t rollup = PROFILE_SEG_OK;
    bool any = false;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(mask & (1u << zi))) {
            continue;
        }
        any = true;
        profile_seg_verdict_t per_zone[PROFILE_MAX_SEGMENTS];
        profile_seg_verdict_t v = profile_feasibility_profile(zi, p, per_zone, PROFILE_MAX_SEGMENTS);
        rollup = worse(rollup, v);
        if (out_segments) {
            for (uint8_t i = 0; i < p->segment_count && i < out_cap && i < PROFILE_MAX_SEGMENTS; i++) {
                out_segments[i] = worse(out_segments[i], per_zone[i]);
            }
        }
    }
    return any ? rollup : PROFILE_SEG_UNKNOWN;
}
