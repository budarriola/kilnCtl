// See zone_coupling_solve.h for why this lives in its own file. Verbatim
// numerics move out of profile_executor.c -- same formulas, same constants,
// same order of float operations -- only the signatures changed, to take
// plain scalars/arrays instead of profile_executor.c's private
// zone_runtime_t* (see the header's doc comment for the substitution table).
#include "zone_coupling_solve.h"

#include <math.h>
#include <string.h>

bool zone_coupling_qualifies_as_neighbor(bool active, bool actual_valid, float actual_c, bool ff_enabled,
                                         float ff_k_dc, zone_control_mode_t control_mode, bool faulted,
                                         bool heat_blocked)
{
    return active && actual_valid && isfinite(actual_c) &&
           ff_enabled && isfinite(ff_k_dc) && ff_k_dc > 0.0f &&
           (control_mode == ZONE_CONTROL_MODE_PID || control_mode == ZONE_CONTROL_MODE_PID_FUZZY) &&
           !faulted && !heat_blocked;
}

uint8_t zone_coupling_count_qualifying_neighbors(uint8_t zi, const zone_coupling_neighbor_t *zones,
                                                 uint8_t zone_count)
{
    float coupling_row[MAX31856_CHANNEL_COUNT];
    uint8_t count = 0;
    if (!zones_config_get_coupling(zi, coupling_row)) {
        return 0;
    }
    for (uint8_t j = 0; j < zone_count; j++) {
        if (j == zi) continue;
        if (isfinite(coupling_row[j]) && coupling_row[j] != 0.0f && zones[j].qualifies) {
            count++;
        }
    }
    return count;
}

void zone_coupling_filter_tick(float *filtered_c, bool *filter_init, float actual_c, bool actual_valid_now,
                               float d_filter_tau_s, float dt_s)
{
    if (!actual_valid_now || !isfinite(actual_c)) {
        return;
    }
    if (!*filter_init) {
        *filtered_c = actual_c;
        *filter_init = true;
        return;
    }
    float tau = d_filter_tau_s;
    if (!(tau > 0.0f)) tau = 30.0f; /* pid.h's documented default, belt-and-braces */
    float alpha = dt_s / (tau + dt_s);
    *filtered_c += alpha * (actual_c - *filtered_c);
}

coupling_solve_reason_t zone_coupling_gauss_solve_partial_pivot(uint8_t n,
                                                                 const float G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT],
                                                                 float b_const, float out_u[MAX31856_CHANNEL_COUNT])
{
    if (n == 0 || n > MAX31856_CHANNEL_COUNT) {
        return COUPLING_SOLVE_FALLBACK_SINGULAR; /* caller contract violation -- treat as untrustworthy */
    }
    float b[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < n; i++) {
        b[i] = b_const;
    }
    return zone_coupling_gauss_solve_partial_pivot_vec(n, G, b, out_u);
}

coupling_solve_reason_t zone_coupling_gauss_solve_partial_pivot_vec(uint8_t n,
                                                                     const float G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT],
                                                                     const float b[MAX31856_CHANNEL_COUNT],
                                                                     float out_u[MAX31856_CHANNEL_COUNT])
{
    if (n == 0 || n > MAX31856_CHANNEL_COUNT) {
        return COUPLING_SOLVE_FALLBACK_SINGULAR; /* caller contract violation -- treat as untrustworthy */
    }

    float M[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT + 1];
    float scale = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            float v = G[i][j];
            if (!isfinite(v)) {
                return COUPLING_SOLVE_FALLBACK_NONFINITE;
            }
            M[i][j] = v;
            float av = fabsf(v);
            if (av > scale) scale = av;
        }
        if (!isfinite(b[i])) {
            return COUPLING_SOLVE_FALLBACK_SINGULAR; /* non-finite target -- same outcome the scalar
                                                       * form's !isfinite(b_const) check produced */
        }
        M[i][n] = b[i];
    }
    if (!(scale > 0.0f)) {
        return COUPLING_SOLVE_FALLBACK_SINGULAR; /* all-zero/degenerate matrix (n==0 already rejected above) */
    }
    float rel_eps = scale * COUPLING_SOLVE_PIVOT_REL_EPS;
    float pivot_floor = (rel_eps > COUPLING_SOLVE_PIVOT_ABS_EPS) ? rel_eps : COUPLING_SOLVE_PIVOT_ABS_EPS;

    for (uint8_t k = 0; k < n; k++) {
        uint8_t piv = k;
        float best = fabsf(M[k][k]);
        for (uint8_t r = (uint8_t)(k + 1); r < n; r++) {
            float av = fabsf(M[r][k]);
            if (av > best) {
                best = av;
                piv = r;
            }
        }
        if (best < pivot_floor) {
            return COUPLING_SOLVE_FALLBACK_SINGULAR;
        }
        if (piv != k) {
            for (uint8_t c = k; c <= n; c++) {
                float tmp = M[k][c];
                M[k][c] = M[piv][c];
                M[piv][c] = tmp;
            }
        }
        for (uint8_t r = (uint8_t)(k + 1); r < n; r++) {
            /* isfinite(factor) deliberately not re-checked here -- see this
             * function's own doc comment (zone_coupling_solve.h) for the
             * bound proof: after the swap above, M[k][k] is the column
             * maximum, so |factor| = |M[r][k]/M[k][k]| <= 1 unconditionally. */
            float factor = M[r][k] / M[k][k];
            for (uint8_t c = k; c <= n; c++) {
                M[r][c] -= factor * M[k][c];
            }
        }
    }

    for (int i = (int)n - 1; i >= 0; i--) {
        float sum = M[i][n];
        for (uint8_t j = (uint8_t)(i + 1); j < n; j++) {
            sum -= M[i][j] * out_u[j];
        }
        /* M[i][i]'s own floor re-check deliberately not repeated here -- it
         * was already validated as the pivot at forward-elimination step
         * k=i, and back-substitution's row operations only ever touch
         * M[i][n] (the RHS column), so M[i][i] is provably unchanged. */
        float v = sum / M[i][i];
        if (!isfinite(v)) {
            return COUPLING_SOLVE_FALLBACK_NONFINITE;
        }
        out_u[i] = v;
    }
    return COUPLING_SOLVE_OK;
}

/* Opus review, blocker 2 (membership-transition damping): the SET of zones
 * qualifying for zi's system can change every tick -- heat_blocked in
 * particular is refreshed unconditionally every tick by
 * profile_executor.c's apply_relay(), so a marginal interlock or an OTA
 * heat-block flapping can flip a neighbour's qualification at tick rate. A
 * membership change steps the WHOLE hold term with nothing to damp it.
 * Tracked as a signature independent of the (members,G,b) cache -- the cache
 * only updates on a miss, but a membership change must be caught on EVERY
 * call, hit or miss, since it is the transition itself that matters, not
 * whether the coefficients also happened to change. Bit 8 records "zi itself
 * qualifies"; bits 0..MAX31856_CHANNEL_COUNT-1 record which OTHER zones are
 * in the system -- together they distinguish every case
 * coupling_solve_reason_t does at the membership level (OUT_OF_RANGE is
 * never reached past the qualifies check so it never reaches this signature
 * at all, by construction below). */
#define COUPLING_MEMBERSHIP_QUALIFIES_BIT 0x0100u

static bool coupling_note_membership_signature(uint16_t *prev_membership_sig, uint16_t sig)
{
    bool changed = *prev_membership_sig != sig;
    *prev_membership_sig = sig;
    return changed;
}

/* PID_EXPANSION_PLAN.md sec 3.2 ("STORAGE LANDED 2026-09-02f" / "the solver
 * switch itself"): the diagonal candidate for member zone `member_zi`.
 * `use_measured` false reproduces the shipped behaviour byte-for-byte --
 * always `fallback_ff_k_dc`, no call to the config layer at all. `use_measured`
 * true tries the matrix's own diagonal cell first, the SAME guarded-fallback
 * shape diagonal_hold/diagonal_climb's own `ff_k_dc` reads already use
 * (zones_config_get_coupling_diag_k_dc() reporting false, or a stored value
 * that is non-finite or <= 0.0f -- zones_http.h's own "not measured"
 * convention for this field -- both fall through to fallback_ff_k_dc).
 *
 * On real hardware only the SECOND half of that guard ever actually fires:
 * zones_config_accessors.c's zones_config_get_coupling_diag_k_dc() returns
 * true for any in-range zone index regardless of whether
 * coupling_diag_k_dc has ever been written (it has no separate "present"
 * flag), and an un-set field default-initializes to 0.0f -- so "never
 * measured" on the board is `measured == 0.0f` with the getter reporting
 * true, not the getter reporting false. The `isfinite(measured) &&
 * measured > 0.0f` checks are therefore load-bearing on their own, not
 * redundant belt-and-braces alongside the return-value check -- see
 * test_zone_coupling_solve.c's guard tests, which pin exactly this. */
static float coupling_diagonal_k_dc(uint8_t member_zi, float fallback_ff_k_dc, bool use_measured)
{
    if (!use_measured) {
        return fallback_ff_k_dc;
    }
    float measured = 0.0f;
    if (zones_config_get_coupling_diag_k_dc(member_zi, &measured) && isfinite(measured) && measured > 0.0f) {
        return measured;
    }
    return fallback_ff_k_dc;
}

float zone_coupling_solve_hold(bool z_qualifies, float z_ff_k_dc, uint8_t zi, bool use_measured_diag_k_dc,
                               const zone_coupling_neighbor_t *zones, uint8_t zone_count,
                               float setpoint_c, float ambient_c, bool *out_used_matrix, bool *out_infeasible,
                               coupling_solve_reason_t *out_reason, bool *out_membership_changed,
                               zone_coupling_hold_cache_t *cache_row, uint16_t *prev_membership_sig)
{
    float diagonal_hold = (setpoint_c - ambient_c) / z_ff_k_dc;
    *out_used_matrix = false;
    *out_infeasible = false;
    *out_membership_changed = false;

    if (zi >= MAX31856_CHANNEL_COUNT || !isfinite(diagonal_hold)) {
        *out_reason = COUPLING_SOLVE_FALLBACK_OUT_OF_RANGE;
        return diagonal_hold; /* out-of-range zi or non-finite input: let the caller's own
                               * isfinite()/clamp belt-and-braces handle it, same as before this fix */
    }
    /* zi must clear the SAME bar every neighbour does -- a faulted/blocked/
     * non-PID-family/inactive zone's own error is not attributable to
     * coupled closed-loop control either, so it has no business being a row
     * in this system (it still gets a feedforward: the diagonal fallback
     * just below). */
    if (!z_qualifies) {
        *out_reason = COUPLING_SOLVE_FALLBACK_UNQUALIFIED;
        *out_membership_changed = coupling_note_membership_signature(prev_membership_sig, 0);
        return diagonal_hold;
    }

    uint8_t members[MAX31856_CHANNEL_COUNT];
    uint8_t n = 0;
    uint16_t membership_sig = COUPLING_MEMBERSHIP_QUALIFIES_BIT;
    members[n++] = zi;
    for (uint8_t j = 0; j < zone_count; j++) {
        if (j == zi) continue;
        if (zones[j].qualifies) {
            members[n++] = j;
            membership_sig |= (uint16_t)(1u << j);
        }
    }
    *out_membership_changed = coupling_note_membership_signature(prev_membership_sig, membership_sig);

    if (n == 1) {
        /* No qualifying neighbours -- the 1x1 system IS (setpoint-ambient)/
         * k_dc, not an approximation of it, so there is nothing to gain by
         * routing it through the general n x n machinery below. */
        *out_reason = COUPLING_SOLVE_FALLBACK_NO_NEIGHBORS;
        return diagonal_hold;
    }

    float G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    memset(G, 0, sizeof(G));
    for (uint8_t row = 0; row < n; row++) {
        uint8_t s = members[row];
        /* PROVENANCE (PID_EXPANSION_PLAN.md sec 3.2, "STORAGE LANDED
         * 2026-09-02f" / "the solver switch itself"): TWO candidate values
         * exist for this cell. `ff_k_dc` (the fallback below) is the per-zone
         * STEP-IDENTIFIED DC gain (autotune_engine.c's single-zone step
         * test) -- what every caller ran exclusively before this flag
         * existed, and still what runs whenever `use_measured_diag_k_dc` is
         * false or the matrix's own cell is not (yet) populated for this
         * zone. The matrix's OWN diagonal cell (`coupling_diag_k_dc`,
         * zones_http.h) is the other candidate: it comes from the SAME
         * rested multi-zone excitation runs as the off-diagonals it sits
         * beside, and sec 3.2's analysis found it better supported by the
         * data (lower condition number, better bias on 2 of 3 zones, wider
         * feasible range) -- but nothing on this board writes it today
         * (hand-set or PC-side preset only, no autotune pass). Sizing the
         * seam between the two choices (the step switching sources makes at
         * the moment a neighbour joins/leaves the coupled system) is
         * test_zone_coupling_solve.c's job; see that file and sec 3.2 for
         * the numbers. */
        float fallback_k_dc_s = (s == zi) ? z_ff_k_dc : zones[s].ff_k_dc;
        G[row][row] = coupling_diagonal_k_dc(s, fallback_k_dc_s, use_measured_diag_k_dc);
        float coupling_row[MAX31856_CHANNEL_COUNT];
        if (!zones_config_get_coupling(s, coupling_row)) {
            continue; /* no row at all for this zone -- every off-diagonal in it stays 0,
                      * degrading exactly to "no coupling measured" for this zone's contribution */
        }
        for (uint8_t col = 0; col < n; col++) {
            uint8_t t = members[col];
            if (t == s) continue; /* diagonal already set above */
            float c = coupling_row[t];
            /* 0.0f is coupling_coeff[]'s own documented "not measured"
             * default (zones_http.c), not a real zero coupling -- the exact
             * same convention the deviation term in profile_executor.c's
             * zone_feedforward() already relies on. Leaving G[row][col] at
             * its memset 0 for an unmeasured/non-finite cell is the correct
             * degrade: "no data" and "measured zero" are indistinguishable
             * in this store, and treating unmeasured as zero is no new
             * assumption -- it is exactly what independent per-zone
             * division already implied for every zone before this fix. */
            if (isfinite(c)) {
                G[row][col] = c;
            }
        }
    }

    /* LOAD-BEARING INVARIANT (Opus review): b_const is a single scalar,
     * because every row of dT uses the SAME setpoint_c -- correct today only
     * because profile_executor.c's s_exec.target_c is one shared value
     * across every zone (no per-zone setpoint trim exists yet). This
     * function's signature takes one scalar setpoint_c, not a per-member
     * array, so a per-zone setpoint literally CANNOT be expressed through
     * this interface as it stands -- the blind spot is in the call contract,
     * not just in what today's tests happen to cover. The day per-zone trim
     * lands, this line (and the signature above it) is exactly where it must
     * change to dT[row] = per_zone_setpoint[members[row]] - ambient_c;
     * nobody should be able to add that feature by quietly reusing zi's own
     * setpoint_c for every row. */
    float b_const = setpoint_c - ambient_c;
    zone_coupling_hold_cache_t *cache = cache_row;
    bool cache_hit = cache->have && cache->n == n && cache->b == b_const &&
                      memcmp(cache->members, members, n) == 0 &&
                      memcmp(cache->G, G, sizeof(G)) == 0;
    if (cache_hit) {
        *out_used_matrix = (cache->reason == COUPLING_SOLVE_OK);
        *out_infeasible = cache->infeasible;
        *out_reason = cache->reason;
        return *out_used_matrix ? cache->u[0] : diagonal_hold;
    }

    float u[MAX31856_CHANNEL_COUNT];
    coupling_solve_reason_t reason = zone_coupling_gauss_solve_partial_pivot(n, G, b_const, u);
    bool infeasible = false;
    if (reason == COUPLING_SOLVE_OK) {
        for (uint8_t i = 0; i < n; i++) {
            if (u[i] > 1.0f) {
                u[i] = 1.0f;
                infeasible = true;
            }
            /* u[i] < 0 left as-is -- see this function's doc comment
             * (zone_coupling_solve.h, "b < 0 case"): not infeasible, and
             * clamping it here would diverge from the legacy formula's own
             * unclamped negative. */
        }
    }

    cache->have = true;
    cache->n = n;
    memcpy(cache->members, members, n);
    memcpy(cache->G, G, sizeof(G));
    cache->b = b_const;
    cache->reason = reason;
    cache->infeasible = (reason == COUPLING_SOLVE_OK) && infeasible;
    if (reason == COUPLING_SOLVE_OK) {
        memcpy(cache->u, u, sizeof(float) * n);
    }

    *out_used_matrix = (reason == COUPLING_SOLVE_OK);
    *out_infeasible = (reason == COUPLING_SOLVE_OK) && infeasible;
    *out_reason = reason;
    return (reason == COUPLING_SOLVE_OK) ? u[0] : diagonal_hold; /* members[0] is always zi */
}

/* See zone_coupling_solve.h's doc comment for the derivation and why this
 * reuses zone_coupling_gauss_solve_partial_pivot_vec() rather than a second
 * solver. Structurally this mirrors zone_coupling_solve_hold() line for line
 * -- same qualification gate, same membership-building loop, same G
 * assembly -- deliberately: any divergence between the two membership sets
 * would mean the hold and climb terms are being computed for two different
 * coupled systems on the same tick, which would make their sum meaningless.
 * The one substantive difference is b: a per-member array (rate_c_per_s *
 * that member's own tau) instead of one scalar shared by every row. */
float zone_coupling_solve_climb(bool z_qualifies, float z_ff_k_dc, float z_ff_tau_s, uint8_t zi,
                                bool use_measured_diag_k_dc,
                                const zone_coupling_neighbor_t *zones, uint8_t zone_count,
                                float rate_c_per_s, bool *out_used_matrix, bool *out_infeasible,
                                coupling_solve_reason_t *out_reason, bool *out_membership_changed,
                                zone_coupling_climb_cache_t *cache_row, uint16_t *prev_membership_sig)
{
    float diagonal_climb = (rate_c_per_s * z_ff_tau_s) / z_ff_k_dc; /* legacy per-zone formula --
                                                                     * profile_executor.c's original
                                                                     * `(rate_c_per_s * z->ff_tau_s) /
                                                                     * z->ff_k_dc`, unchanged */
    *out_used_matrix = false;
    *out_infeasible = false;
    *out_membership_changed = false;

    if (zi >= MAX31856_CHANNEL_COUNT || !isfinite(diagonal_climb)) {
        *out_reason = COUPLING_SOLVE_FALLBACK_OUT_OF_RANGE;
        return diagonal_climb;
    }
    if (!z_qualifies) {
        *out_reason = COUPLING_SOLVE_FALLBACK_UNQUALIFIED;
        *out_membership_changed = coupling_note_membership_signature(prev_membership_sig, 0);
        return diagonal_climb;
    }

    uint8_t members[MAX31856_CHANNEL_COUNT];
    uint8_t n = 0;
    uint16_t membership_sig = COUPLING_MEMBERSHIP_QUALIFIES_BIT;
    members[n++] = zi;
    for (uint8_t j = 0; j < zone_count; j++) {
        if (j == zi) continue;
        if (zones[j].qualifies) {
            members[n++] = j;
            membership_sig |= (uint16_t)(1u << j);
        }
    }
    *out_membership_changed = coupling_note_membership_signature(prev_membership_sig, membership_sig);

    if (n == 1) {
        *out_reason = COUPLING_SOLVE_FALLBACK_NO_NEIGHBORS;
        return diagonal_climb;
    }

    float G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    memset(G, 0, sizeof(G));
    float b[MAX31856_CHANNEL_COUNT];
    for (uint8_t row = 0; row < n; row++) {
        uint8_t s = members[row];
        float fallback_k_dc_s = (s == zi) ? z_ff_k_dc : zones[s].ff_k_dc;
        float tau_s = (s == zi) ? z_ff_tau_s : zones[s].ff_tau_s;
        G[row][row] = coupling_diagonal_k_dc(s, fallback_k_dc_s, use_measured_diag_k_dc);
        b[row] = rate_c_per_s * tau_s; /* NOT uniform across rows -- see this function's own doc
                                        * comment and the header's "LOAD-BEARING INVARIANT" note on
                                        * zone_coupling_solve_hold(), which this deliberately does
                                        * NOT copy: tau differs per zone even though setpoint/ambient
                                        * do not. */
        float coupling_row[MAX31856_CHANNEL_COUNT];
        if (!zones_config_get_coupling(s, coupling_row)) {
            continue;
        }
        for (uint8_t col = 0; col < n; col++) {
            uint8_t t = members[col];
            if (t == s) continue;
            float c = coupling_row[t];
            if (isfinite(c)) {
                G[row][col] = c;
            }
        }
    }

    zone_coupling_climb_cache_t *cache = cache_row;
    bool cache_hit = cache->have && cache->n == n &&
                      memcmp(cache->members, members, n) == 0 &&
                      memcmp(cache->b, b, sizeof(float) * n) == 0 &&
                      memcmp(cache->G, G, sizeof(G)) == 0;
    if (cache_hit) {
        *out_used_matrix = (cache->reason == COUPLING_SOLVE_OK);
        *out_infeasible = cache->infeasible;
        *out_reason = cache->reason;
        return *out_used_matrix ? cache->u[0] : diagonal_climb;
    }

    float u[MAX31856_CHANNEL_COUNT];
    coupling_solve_reason_t reason = zone_coupling_gauss_solve_partial_pivot_vec(n, G, b, u);
    bool infeasible = false;
    if (reason == COUPLING_SOLVE_OK) {
        for (uint8_t i = 0; i < n; i++) {
            if (u[i] > 1.0f) {
                u[i] = 1.0f;
                infeasible = true;
            }
            /* u[i] < 0 (a cooling-direction ramp) left unclamped here too --
             * same rationale as zone_coupling_solve_hold(), combined with the
             * (also possibly negative) hold term and clamped only in the
             * caller's final [0,1] sum. */
        }
    }

    cache->have = true;
    cache->n = n;
    memcpy(cache->members, members, n);
    memcpy(cache->G, G, sizeof(G));
    memcpy(cache->b, b, sizeof(float) * n);
    cache->reason = reason;
    cache->infeasible = (reason == COUPLING_SOLVE_OK) && infeasible;
    if (reason == COUPLING_SOLVE_OK) {
        memcpy(cache->u, u, sizeof(float) * n);
    }

    *out_used_matrix = (reason == COUPLING_SOLVE_OK);
    *out_infeasible = (reason == COUPLING_SOLVE_OK) && infeasible;
    *out_reason = reason;
    return (reason == COUPLING_SOLVE_OK) ? u[0] : diagonal_climb;
}
