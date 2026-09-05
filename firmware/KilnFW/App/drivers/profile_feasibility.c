#include "profile_feasibility.h"

#include <math.h>

#include "zones_config_accessors.h"

/* A feasibility colour that changed while the user was looking at it -- the
 * same segment reading OK, then UNREACHABLE, then OK again as they sat on
 * the profile editor page -- would be worse than a slightly stale one: it
 * teaches the operator that the colour is noise, not a verdict, and the
 * whole point of this module is that the colour can be trusted. This is why
 * the ambient this module uses is the ZONE'S OWN PERSISTED tuning-quality
 * baseline_c (get_persisted_ambient_c() below), not a fresh live
 * reading: it only changes when that zone is re-autotuned, which is a
 * deliberate, infrequent, operator-visible event, never a background poll
 * racing the page the user is reading.
 *
 * Fallback ambient for the plant model, used whenever no persisted ambient
 * is available for a zone (never autotuned, a getter that cannot answer, or
 * a stored value that is non-finite or outside the sane range below). Also
 * the value profile_executor.c falls back to when its own step_ambient_c
 * capture never happened. Kept as a named constant rather than folded into
 * get_persisted_ambient_c() because zero-tuned/no-thermocouple boards need
 * SOME number, and 20 C is a reasonable room-temperature guess for that
 * case. */
#define FEASIBILITY_AMBIENT_C 20.0f

/* Persisted ambient is required to fall in this range before it is trusted --
 * a corrupt or pre-migration record handing this module something like -40
 * or 150 should not be allowed to silently make every profile look reachable
 * or nothing look reachable. The lower bound is deliberately above 0: 0.0 is
 * also the C zero-init value every non-autotuned/relay-only zone's
 * zone_tuning_quality_t fields read as, so a bound starting at 0 could not
 * tell a real (if unusually cold) reading apart from that sentinel -- see
 * get_persisted_ambient_c()'s own comment for the record this was written
 * against (7e632e8's review finding 1). A genuinely sub-5C shop is
 * implausible (this is a kiln autotune baseline, not an outdoor sensor), so
 * such a record is treated as untrustworthy and falls back to
 * FEASIBILITY_AMBIENT_C (20.0f) -- which raises the computed ceiling rather
 * than lowering it. Accepted: a shop that cold is not a case this module
 * needs to serve correctly. */
#define FEASIBILITY_AMBIENT_MIN_C 5.0f
#define FEASIBILITY_AMBIENT_MAX_C 60.0f

/* Persisted per-zone ambient the fitted model (zones_config_get_model()) is
 * actually relative to: q.baseline_c, the settled chamber temperature
 * autotune measured immediately before that zone's identification step began
 * (fopdt_model_t::baseline_c, pid_autotune.h) -- NOT q.step_ambient_c, which
 * is a cold-junction reading of the board itself. The two are close but not
 * identical (the CJ sits 1.6-2.0 C below chamber per
 * autotune_engine_step_identify.c:920) and, critically, baseline_c is the
 * SAME reference frame k_dc was fitted relative to
 * (autotune_finalize_fit()/pid_autotune_fit_fopdt(): raw_rise_c = final_c -
 * baseline_c). Using step_ambient_c here would silently mix two different
 * zero points into "T_amb + k_dc" and skew the ceiling by that 1.6-2.0 C gap.
 * A self-heated live cold-junction reading is rejected for the same reason
 * documented previously: it would inflate the computed ceiling precisely
 * when a firing is running and the check matters most, and this module
 * intentionally never reads live temperature at all.
 *
 * Requires q.valid AND q.method == AUTOTUNE_METHOD_STEP: zones_config_
 * set_tuning_quality() is (today) only ever called from the STEP-method
 * accept path (autotune_engine_guard.c -- the RELAY branch returns before
 * that call is reached, since a relay test fits no FOPDT model to attach a
 * quality record to), so this check is currently always true for any record
 * with valid=true. It is kept anyway as defence in depth against a relay (or
 * future non-STEP) method ever reaching that call site with baseline_c left
 * at its 0.0f struct-init value (autotune_engine.c zeroes step_ambient_c at
 * run start, ~line 1090, and only the SETTLING->STEPPING transition for a
 * STEP run, ~line 597, ever gives it a real value) -- 0.0 passes the
 * MIN/MAX range check above as easily as a genuine near-zero reading would,
 * so the method gate is the only thing standing between that sentinel and a
 * ceiling that is silently 20 C too low. Also requires baseline_c itself to
 * be finite and in range; any failure of any of these falls back to
 * FEASIBILITY_AMBIENT_C. Pure config lookup -- no I/O, no lock, no
 * cached/stale state to reason about.
 *
 * FEASIBILITY_TUNING_METHOD_STEP (used just below) is autotune_method_t's
 * AUTOTUNE_METHOD_STEP (autotune_engine.h) as a literal, declared in
 * profile_feasibility.h rather than pulling that header's whole
 * state-machine/FreeRTOS-handle surface into this module just for one
 * enumerator -- see the header for the compile-time assert that keeps the two
 * pinned together. zone_tuning_quality_t::method's own doc comment
 * (zones_http.h) documents this exact encoding ("autotune_method_t raw
 * value: 0=STEP, 1=RELAY") for the same reason. */
static float get_persisted_ambient_c(uint8_t zone_index)
{
    zone_tuning_quality_t q;
    if (zones_config_get_tuning_quality(zone_index, &q) && q.valid &&
        q.method == FEASIBILITY_TUNING_METHOD_STEP &&
        isfinite(q.baseline_c) && q.baseline_c >= FEASIBILITY_AMBIENT_MIN_C &&
        q.baseline_c <= FEASIBILITY_AMBIENT_MAX_C) {
        return q.baseline_c;
    }
    return FEASIBILITY_AMBIENT_C;
}

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

/* Forward declaration -- the actual verdict logic, taking t_amb as a
 * parameter rather than sampling it itself. See profile_feasibility_segment_in_mask()
 * and profile_feasibility_profile_in_mask() below for the two public entry
 * points that sample it once and pass it down. */
static profile_seg_verdict_t segment_verdict(uint8_t zone_index, uint8_t zone_mask, float start_c,
                                             const profile_segment_t *seg, float t_amb);

/* Effective full-power steady-state gain for zone `zone_index` when the run
 * drives every zone in `zone_mask` at once.
 *
 * WHY THIS EXISTS: zones_config_get_model()'s k_dc was fitted by stepping ONE
 * zone with the others idle, so T_ss - T_amb = k_dc describes a solo firing
 * only. The zones share a chamber and heat each other, which the persisted
 * coupling matrix measures: zones_config_get_coupling(i, row) fills row[j]
 * with the steady-state rise in degrees C seen by zone i (the AFFECTED zone)
 * per unit of actuation applied to zone j (the STEPPED zone). At u = 1 for
 * every zone in the mask, zone i therefore settles at
 *
 *   T_ss(i) = T_amb + k_dc[i] + sum over j in mask, j != i, of c[i][j]
 *
 * so k_eff = k_dc + that sum is the gain both the ceiling test and the
 * heating-headroom rate test must use. Judging a three-zone profile on solo
 * k_dc alone marked genuinely firable schedules UNREACHABLE -- `cplval70`
 * (70 C, zone_mask 7) was flagged red because zones 1 and 2 each top out near
 * 52 C on their own, while all three together reach 108/88/72 C.
 *
 * The diagonal is skipped (it is k_dc's own job) and zones outside the mask
 * contribute nothing -- they are not being driven. A missing matrix, a getter
 * that cannot answer, or a non-finite cell contributes zero, which reproduces
 * exactly the pre-coupling behaviour: this can only ever raise the ceiling,
 * never lower it below what the solo model already promised.
 *
 * The sum assumes u = 1 on every OTHER masked zone too, so k_eff is an UPPER
 * bound on the real coupling contribution, not a guarantee: a neighbour that
 * throttles back once it reaches its own dwell setpoint stops delivering its
 * row[j] share, so a profile whose verdict sits close to this ceiling may
 * still fail to hold it in practice. Advisory, not a promise -- the same way
 * the solo k_dc ceiling always has been.
 *
 * tau_s is deliberately NOT adjusted: the coupling row's own tau
 * (zones_config_get_coupling_tau()) describes how fast a neighbour's heat
 * arrives, which shifts the transient, while this module's only question is
 * the sustained rate a segment needs. Same reasoning as dead_time_s below. */
static float effective_k_dc(uint8_t zone_index, uint8_t zone_mask, float k_dc)
{
    float row[MAX31856_CHANNEL_COUNT];
    if (!zones_config_get_coupling(zone_index, row)) {
        return k_dc;
    }
    float k_eff = k_dc;
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (j == zone_index || !(zone_mask & (1u << j))) {
            continue;
        }
        if (!isfinite(row[j]) || row[j] <= 0.0f) {
            continue;
        }
        k_eff += row[j];
    }
    return k_eff;
}

profile_seg_verdict_t profile_feasibility_segment(uint8_t zone_index, float start_c,
                                                  const profile_segment_t *seg)
{
    /* Solo judgement: the mask names only this zone, so no neighbour is
     * driven and no coupling contribution applies. */
    return profile_feasibility_segment_in_mask(zone_index, (uint8_t)(1u << zone_index), start_c,
                                               seg);
}

profile_seg_verdict_t profile_feasibility_segment_in_mask(uint8_t zone_index, uint8_t zone_mask,
                                                          float start_c,
                                                          const profile_segment_t *seg)
{
    /* Public entry point -- ambient is sampled exactly once here and threaded
     * down to segment_verdict(), rather than each internal step reading its
     * own copy (the persisted getter is deterministic and I/O-free, so two
     * calls would in practice agree, but review of the earlier live-ambient
     * version flagged exactly this pattern -- start_c and t_amb sampled at
     * two different points -- as fragile, so it is not repeated here even
     * though it is no longer live I/O). */
    return segment_verdict(zone_index, zone_mask, start_c, seg, get_persisted_ambient_c(zone_index));
}

static profile_seg_verdict_t segment_verdict(uint8_t zone_index, uint8_t zone_mask, float start_c,
                                             const profile_segment_t *seg, float t_amb)
{
    if (!seg) {
        return PROFILE_SEG_UNKNOWN;
    }
    if (!(zone_mask & (1u << zone_index))) {
        /* zone_index isn't even in the mask being judged -- it is not being
         * driven at all, so there is nothing to answer about it. See the
         * header for why this is UNKNOWN rather than OK or a hard error. */
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

    /* Gain the whole run actually delivers to this zone, neighbours included
     * -- see effective_k_dc() above for the model and why it matters. */
    const float k_eff = effective_k_dc(zone_index, zone_mask, k_dc);

    const float ceiling_c = t_amb + k_eff; /* steady state at u = 1: T - T_amb = K_eff */

    float target = seg->target_c;
    if (!isfinite(target) || !isfinite(start_c)) {
        return PROFILE_SEG_UNKNOWN;
    }

    /* Steady-state ceiling. Checked before the rate, and reported as its own
     * verdict, because it is the more serious failure: TOO_FAST means "slow
     * this segment down and it works", UNREACHABLE means the kiln will sit
     * below the target at full power until the operator gives up. */
    if (target >= start_c && target > ceiling_c - FEASIBILITY_CEILING_MARGIN_C) {
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
        float headroom = k_eff - (worst_t - t_amb);
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
    return profile_feasibility_profile_in_mask(zone_index, (uint8_t)(1u << zone_index), p,
                                               out_segments, out_cap);
}

profile_seg_verdict_t profile_feasibility_profile_in_mask(uint8_t zone_index, uint8_t zone_mask,
                                                          const profile_t *p,
                                                          profile_seg_verdict_t *out_segments,
                                                          size_t out_cap)
{
    if (!p || p->segment_count == 0) {
        return PROFILE_SEG_UNKNOWN;
    }

    /* Public entry point -- ambient is sampled exactly once here, used both
     * as the starting temperature (the run starts from the room) and passed
     * down to every segment_verdict() call below, so all segments of this
     * one profile walk see the identical value even though each segment
     * call happens at a different point in the loop. */
    const float t_amb = get_persisted_ambient_c(zone_index);
    float start_c = t_amb;
    profile_seg_verdict_t rollup = PROFILE_SEG_OK;

    for (uint8_t i = 0; i < p->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        profile_seg_verdict_t v =
            segment_verdict(zone_index, zone_mask, start_c, &p->segments[i], t_amb);
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
        /* `mask`, not `zone_mask`: a builtin's incoming zone_mask of 0 has
         * already been resolved to every configured zone above. That
         * resolved set is also the set the executor will actually run the
         * profile on: profiles_http.c resolves a builtin's mask to
         * (1u<<thermo_count)-1 before invoking the executor, so a builtin
         * always runs on every configured zone together, never solo. The
         * coupled judgement is therefore correct for mask 0 exactly as it is
         * for an explicit nonzero zone_mask -- there is no zone-agnostic
         * case that should be judged SOLO. */
        uint8_t coupling_mask = mask;
        profile_seg_verdict_t v = profile_feasibility_profile_in_mask(zi, coupling_mask, p,
                                                                      per_zone, PROFILE_MAX_SEGMENTS);
        rollup = worse(rollup, v);
        if (out_segments) {
            for (uint8_t i = 0; i < p->segment_count && i < out_cap && i < PROFILE_MAX_SEGMENTS; i++) {
                out_segments[i] = worse(out_segments[i], per_zone[i]);
            }
        }
    }
    return any ? rollup : PROFILE_SEG_UNKNOWN;
}
