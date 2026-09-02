// adaptive_tune_model.c -- Layer 2/3 of adaptive_tune.c's split (2026-09-01,
// see adaptive_tune_internal.h's own top comment for the full shape): the
// per-zone diagonal K_dc refine (try_refine_zone_locked()) and the full
// coupled identification (adaptive_tune_coupled_fit(), try_refine_coupled_
// locked()). Moved out of adaptive_tune.c unchanged -- no behavior here is
// new except where a comment says so.
#include "adaptive_tune_internal.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "pid_autotune.h"
#include "zone_coupling_solve.h" // zone_coupling_gauss_solve_partial_pivot_vec() -- the ONLY thing of this
                                  // module's this file calls; never edited here (see PID_EXPANSION_PLAN.md
                                  // 3.3 and this file's coupled-fit comments for why reusing its conditioning
                                  // check is load-bearing, not cosmetic)
#include "zones_http.h" // zones_config_get_model/set_model/get_pid/set_pid/get_coupling/set_coupling_cell

// ---------------------------------------------------------------------
// Layer 2/3 -- batch fit, blend, guard, and apply -- called only from
// adaptive_tune_run_end(), i.e. only ever at a run boundary. This is what
// makes a mid-firing bump structurally impossible: there is no other call
// site that can reach zones_config_set_model()/set_pid() from this module.
// ---------------------------------------------------------------------

// Returns true only on the happy path where zones_config_set_model()/
// set_pid() actually ran -- i.e. Kp/Ki/Kd were just rewritten this run from
// the SIMC recompute. adaptive_tune_run_end() uses this to decide whether
// try_refine_ki_locked() may run this same run -- see that call site's own
// comment (D5).
bool try_refine_zone_locked(uint8_t zi, uint8_t profile_id)
{
    adaptive_tune_zone_t *z = &s_at_zones[zi];

    if (z->ring_count < ADAPTIVE_TUNE_MIN_OBSERVATIONS) {
        adaptive_tune_set_refusal(z, "only %u/%u dwell observations", (unsigned)z->ring_count,
                    (unsigned)ADAPTIVE_TUNE_MIN_OBSERVATIONS);
        return false;
    }

    float duty[ADAPTIVE_TUNE_RING_CAPACITY], rise[ADAPTIVE_TUNE_RING_CAPACITY];
    float umin = INFINITY, umax = -INFINITY;
    for (uint32_t i = 0; i < z->ring_count; i++) {
        uint32_t idx = (z->ring_head + i) % ADAPTIVE_TUNE_RING_CAPACITY;
        duty[i] = z->ring[idx].duty;
        rise[i] = z->ring[idx].rise_c;
        if (duty[i] < umin) umin = duty[i];
        if (duty[i] > umax) umax = duty[i];
    }
    if ((umax - umin) < ADAPTIVE_TUNE_MIN_DUTY_SPREAD) {
        adaptive_tune_set_refusal(z, "observations too clustered (duty spread %.3f < %.3f)", (double)(umax - umin),
                    (double)ADAPTIVE_TUNE_MIN_DUTY_SPREAD);
        return false;
    }

    float k_fit;
    if (!adaptive_tune_fit_gain(duty, rise, z->ring_count, &k_fit)) {
        adaptive_tune_set_refusal(z, "fit degenerate (insufficient duty energy)");
        return false;
    }
    if (!(k_fit > 0.0f)) {
        adaptive_tune_set_refusal(z, "fitted gain %.4f is not positive", (double)k_fit);
        return false;
    }

    float k_dc, tau_s, dead_time_s;
    if (!zones_config_get_model(zi, &k_dc, &tau_s, &dead_time_s) || !(k_dc > 0.0f)) {
        adaptive_tune_set_refusal(z, "no existing step-test model -- learning refines, it does not create one");
        return false;
    }

    if (k_fit > k_dc * ADAPTIVE_TUNE_MAX_JUMP_RATIO || k_fit < k_dc / ADAPTIVE_TUNE_MAX_JUMP_RATIO) {
        adaptive_tune_set_refusal(z, "fit %.4f is implausible against prior K %.4f (>%.0fx)", (double)k_fit, (double)k_dc,
                    (double)ADAPTIVE_TUNE_MAX_JUMP_RATIO);
        return false;
    }

    float k_blended = k_dc + ADAPTIVE_TUNE_BLEND_ALPHA * (k_fit - k_dc);
    float max_move = k_dc * ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE;
    if (k_blended > k_dc + max_move) k_blended = k_dc + max_move;
    if (k_blended < k_dc - max_move) k_blended = k_dc - max_move;
    if (!(k_blended > 0.0f)) {
        adaptive_tune_set_refusal(z, "blended gain %.4f is not positive", (double)k_blended);
        return false;
    }

    // F1: refuse rather than write a blend too small to be a material
    // change -- see ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC's own comment.
    float material_move = fabsf(k_blended - k_dc);
    if (material_move < k_dc * ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC) {
        adaptive_tune_set_refusal(z, "blended gain %.4f is not a material change from prior %.4f (<%.2f%%)", (double)k_blended,
                    (double)k_dc, (double)(ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC * 100.0f));
        return false;
    }

    // Recompute PID gains through the SAME rule autotune's Accept path
    // uses (pid_autotune.c's pid_autotune_tune_from_fopdt(), SIMC, default
    // lambda) -- never a second, looser tuning formula. tau_s/dead_time_s
    // are carried over UNCHANGED: dwell data cannot inform dynamics (see
    // adaptive_tune.h's scope note), only the gain.
    fopdt_model_t model = {
        .k_gain_c_per_duty = k_blended,
        .tau_s = tau_s,
        .dead_time_s = dead_time_s,
        .valid = true,
        .settled = true,
        .tau_consistent_with_gain = true,
        .extrapolation_converged = true,
    };
    autotune_gains_t gains = pid_autotune_tune_from_fopdt(&model, AUTOTUNE_RULE_SIMC, 0.0f);
    if (gains.refusal != AUTOTUNE_REFUSAL_OK) {
        adaptive_tune_set_refusal(z, "SIMC refused the refined model: %s", gains.refusal_reason);
        return false;
    }

    if (!zones_config_set_model(zi, k_blended, tau_s, dead_time_s)) {
        adaptive_tune_set_refusal(z, "zones_config_set_model() rejected %.4f/%.1f/%.1f", (double)k_blended, (double)tau_s,
                    (double)dead_time_s);
        return false;
    }
    if (!zones_config_set_pid(zi, gains.kp, gains.ki, gains.kd)) {
        adaptive_tune_set_refusal(z, "zones_config_set_pid() rejected %.4f/%.4f/%.4f", (double)gains.kp, (double)gains.ki,
                    (double)gains.kd);
        return false;
    }

    z->last_refusal_reason[0] = '\0';
    z->has_applied = true;
    z->prior_k_dc = k_dc;
    z->applied_k_dc = k_blended;
    z->last_delta_pct = (k_dc > 0.0f) ? ((k_blended - k_dc) / k_dc) * 100.0f : 0.0f;
    z->last_applied_profile_id = profile_id;
    z->last_applied_unix_s = (uint32_t)time(NULL);

    // Q4: re-latch the Ki-diagnosis baseline (adaptive_tune_ki.c) to the Ki
    // this SIMC recompute just wrote -- see try_refine_ki_locked()'s own
    // comment on ki_baseline for the full "which layer is the more
    // authoritative reference" reasoning. This is the ONLY setter this
    // module writes gains.ki through, so this is the one place a fresh SIMC
    // Ki exists to latch from. adaptive_tune_run_end() (adaptive_tune.c)
    // detects this write and dispatches the NVS persist the same way it
    // already does for adaptive_tune_ki.c's own latch -- see that function's
    // baseline_newly_latched comment.
    z->ki_baseline = gains.ki;
    z->ki_baseline_valid = true;

    ESP_LOGI(ADAPTIVE_TUNE_TAG, "zone %u: K_dc %.4f -> %.4f (%.1f%%) from %u observations, profile %u", (unsigned)zi,
             (double)k_dc, (double)k_blended, (double)z->last_delta_pct, (unsigned)z->ring_count,
             (unsigned)profile_id);
    return true;
}

// ---------------------------------------------------------------------
// Full coupled identification -- pure fit (host-tested directly) plus its
// locked apply helper. See adaptive_tune.h's own comment on adaptive_tune_
// coupled_fit() for the orientation contract: out_C[affected][stepped],
// i.e. the SAME row/column convention zones_config_get/set_coupling() and
// PID_EXPANSION_PLAN.md section 2 both use for persisted storage -- NOT
// /api/autotune/matrix's transposed wire form. This function never touches
// the wire form at all; it writes straight through zones_config_set_
// coupling_cell(affected, stepped, ...), so there is no transpose step for
// this code to get backwards.
//
// Derivation: at every joint dwell observation k, for affected zone i,
//   rise_obs[k][i] == actual_c_i - ambient_c == sum_j coupling_coeff[i][j] * duty_obs[k][j]
// (the coupled steady-state relation zone_coupling_solve_hold() SOLVES at
// runtime, given a known matrix, for duty -- this is its INVERSE problem:
// given many (duty, rise) pairs, solve for the matrix). That is m linear
// equations in n unknowns (coupling_coeff[i][0..n-1]) for row i -- ordinary
// least squares, via the normal equations (duty_obs^T * duty_obs) * row_i =
// duty_obs^T * rise_obs[:,i]. The design matrix (duty_obs^T * duty_obs) is
// the SAME n x n matrix for every row i (only the right-hand side changes
// per affected zone), so it is built once and n independent vector solves
// are run against it -- reusing zone_coupling_gauss_solve_partial_pivot_
// vec() (zone_coupling_solve.c, called, never edited) for the actual
// elimination, which is what gives this function its conditioning check
// for free: COUPLING_SOLVE_FALLBACK_SINGULAR/NONFINITE from that call
// becomes this function's ILL_CONDITIONED refusal, at that function's own
// documented bound (COUPLING_SOLVE_PIVOT_REL_EPS = 1e-4 relative pivot
// floor, admitting condition numbers up to ~1e4 -- see zone_coupling_
// solve.h's own comment on that constant). This file adds no second,
// independent conditioning heuristic on top of it.
adaptive_tune_coupled_result_t adaptive_tune_coupled_fit(
    const float duty_obs[][MAX31856_CHANNEL_COUNT], const float rise_obs[][MAX31856_CHANNEL_COUNT], uint32_t m,
    uint8_t n, float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT])
{
    if (!duty_obs || !rise_obs || !out_C || n == 0 || n > MAX31856_CHANNEL_COUNT) {
        return ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS;
    }
    if (m < (uint32_t)n + ADAPTIVE_TUNE_COUPLED_OBS_MARGIN) {
        return ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS;
    }

    float AtA[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    memset(AtA, 0, sizeof(AtA));
    for (uint8_t a = 0; a < n; a++) {
        for (uint8_t b = 0; b < n; b++) {
            double s = 0.0;
            for (uint32_t k = 0; k < m; k++) {
                s += (double)duty_obs[k][a] * (double)duty_obs[k][b];
            }
            AtA[a][b] = (float)s;
        }
    }

    for (uint8_t i = 0; i < n; i++) {
        float AtR[MAX31856_CHANNEL_COUNT];
        memset(AtR, 0, sizeof(AtR));
        for (uint8_t a = 0; a < n; a++) {
            double s = 0.0;
            for (uint32_t k = 0; k < m; k++) {
                s += (double)duty_obs[k][a] * (double)rise_obs[k][i];
            }
            AtR[a] = (float)s;
        }
        float x[MAX31856_CHANNEL_COUNT];
        coupling_solve_reason_t reason = zone_coupling_gauss_solve_partial_pivot_vec(n, AtA, AtR, x);
        if (reason != COUPLING_SOLVE_OK) {
            return ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED;
        }
        for (uint8_t j = 0; j < n; j++) {
            out_C[i][j] = x[j];
        }
    }
    return ADAPTIVE_TUNE_COUPLED_OK;
}

void try_refine_coupled_locked(uint8_t zi)
{
    adaptive_tune_zone_t *z = &s_at_zones[zi];
    z->coupled_attempted = true;
    z->coupled_applied = false;
    z->coupled_cells_changed = 0;
    z->joint_observations = s_joint_ring_count;

    const uint8_t n = MAX31856_CHANNEL_COUNT;
    uint32_t min_obs = (uint32_t)n + ADAPTIVE_TUNE_COUPLED_OBS_MARGIN;
    if (s_joint_ring_count < min_obs) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "only %u/%u joint dwell observations for coupled solve", (unsigned)s_joint_ring_count,
                   (unsigned)min_obs);
        return;
    }

    // Stack: ADAPTIVE_TUNE_JOINT_RING_CAPACITY(24) * MAX31856_CHANNEL_COUNT(3)
    // * 4 bytes * 2 arrays = 576 bytes -- same order as this file's other
    // stack-local fit buffers, well inside a FreeRTOS task's normal stack.
    float duty_obs[ADAPTIVE_TUNE_JOINT_RING_CAPACITY][MAX31856_CHANNEL_COUNT];
    float rise_obs[ADAPTIVE_TUNE_JOINT_RING_CAPACITY][MAX31856_CHANNEL_COUNT];
    for (uint32_t k = 0; k < s_joint_ring_count; k++) {
        uint32_t idx = (s_joint_ring_head + k) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
        memcpy(duty_obs[k], s_joint_ring[idx].duty, sizeof(duty_obs[k]));
        memcpy(rise_obs[k], s_joint_ring[idx].rise_c, sizeof(rise_obs[k]));
    }

    float C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty_obs, rise_obs, s_joint_ring_count, n, C);
    if (r == ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "joint observation set degenerate for a determined solve");
        return;
    }
    if (r == ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "joint duty matrix ill-conditioned (cond above ~1e4, zone_coupling_solve.h's own pivot floor)");
        return;
    }

    float prior_row[MAX31856_CHANNEL_COUNT];
    if (!zones_config_get_coupling(zi, prior_row)) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "no existing coupling row to refine");
        return;
    }
    float tau_row[MAX31856_CHANNEL_COUNT], dead_row[MAX31856_CHANNEL_COUNT];
    if (!zones_config_get_coupling_tau(zi, tau_row)) memset(tau_row, 0, sizeof(tau_row));
    if (!zones_config_get_coupling_dead_time(zi, dead_row)) memset(dead_row, 0, sizeof(dead_row));

    uint8_t changed = 0;
    for (uint8_t j = 0; j < n; j++) {
        if (j == zi) {
            continue; // diagonal (this zone's own gain) stays owned by the existing per-zone K_dc
                      // path (try_refine_zone_locked()) -- not duplicated here, see this function's
                      // header comment.
        }
        float fit = C[zi][j];
        if (!isfinite(fit)) {
            continue; // skip only this cell -- do not let one bad column poison the whole row
        }
        float prior = prior_row[j];
        if (prior > ADAPTIVE_TUNE_COUPLING_PRIOR_NEAR_ZERO) {
            // Confident-ish prior -- ratio-based guard, same posture as the
            // diagonal path, EXCEPT the upper bound is widened to the same
            // absolute ceiling the near-zero branch uses below whenever the
            // ratio bound would be tighter than that ceiling. Without this
            // OR, a cell blended up from a near-zero prior (e.g. 0.15*26.6 ~=
            // 3.99 after its first accepted run) becomes a "confident" prior
            // by this branch's own >NEAR_ZERO test, and a 5x ratio around
            // 3.99 (cap ~19.9) then permanently REJECTS the true coefficient
            // (~26.6) on every subsequent run -- a convergence trap for any
            // true value more than ~5.3x the near-zero blend step. Capping
            // fit <= 50 IS the plausibility bound this module has already
            // decided is acceptable for a coupling cell from any starting
            // point (see ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS); reusing it
            // here as a floor under the ratio ceiling lets a cell climb all
            // the way to a true value that far exceeds its early, still-low
            // prior, while the ratio's LOWER bound is left alone -- a
            // confident prior's fit dropping to a small fraction of itself
            // is still refused as implausible in either regime.
            float upper = prior * ADAPTIVE_TUNE_MAX_JUMP_RATIO;
            if (upper < ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS) {
                upper = ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS;
            }
            float lower = prior / ADAPTIVE_TUNE_MAX_JUMP_RATIO;
            if (fit > upper || fit < lower) {
                continue;
            }
        } else if (fabsf(fit) > ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS) {
            // No confident prior (never measured) -- absolute implausibility guard instead.
            continue;
        }

        float blended = prior + ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA * (fit - prior);
        if (blended > prior + ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE) blended = prior + ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE;
        if (blended < prior - ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE) blended = prior - ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE;
        if (blended < 0.0f) blended = 0.0f;                   // storage convention: non-negative
        if (blended > ZONE_COUPLING_COEFF_MAX) blended = ZONE_COUPLING_COEFF_MAX;

        if (zones_config_set_coupling_cell(zi, j, blended, tau_row[j], dead_row[j])) {
            changed++;
        }
    }

    z->coupled_cells_changed = changed;
    if (changed > 0) {
        z->coupled_applied = true;
        z->coupled_refusal_reason[0] = '\0';
        ESP_LOGI(ADAPTIVE_TUNE_TAG, "zone %u: coupled solve refined %u coupling cell(s) from %u joint observations",
                 (unsigned)zi, (unsigned)changed, (unsigned)s_joint_ring_count);
    } else {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "coupled solve succeeded but every off-diagonal cell was implausible or rejected");
    }
}
