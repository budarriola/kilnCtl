// adaptive_tune_model.c -- Layer 2/3 of adaptive_tune.c's split (2026-09-01,
// see adaptive_tune_internal.h's own top comment for the full shape): the
// per-zone diagonal K_dc refine (adaptive_tune_refine_zone_locked()) and the full
// coupled identification (adaptive_tune_coupled_fit(), adaptive_tune_refine_coupled_
// locked()). Moved out of adaptive_tune.c unchanged -- no behavior here is
// new except where a comment says so.
#include "adaptive_tune_internal.h"

#include <math.h>
#include <stdlib.h> // free() of the persist_scratch_alloc() observation buffers in adaptive_tune_plan_coupled_locked()
#include "persist_scratch.h" // persist_scratch_alloc(): PSRAM-first heap scratch, released with free()
#include <string.h>
#include <time.h>

#include "esp_attr.h"  // RTC_NOINIT_ATTR -- adaptive_tune_coupled_breadcrumb_mark()'s storage below
#include "esp_log.h"
#include "freertos/task.h" // uxTaskGetStackHighWaterMark() -- same call stack_margin.c makes, see
                            // stack_margin_calc.h's comment for why the result needs no unit conversion here

#include "pid_autotune.h"
#include "zone_coupling_solve.h" // zone_coupling_gauss_solve_partial_pivot_vec() -- the ONLY thing of this
                                  // module's this file calls; never edited here (see PID_EXPANSION_PLAN.md
                                  // 3.3 and this file's coupled-fit comments for why reusing its conditioning
                                  // check is load-bearing, not cosmetic)
#include "zones_config_accessors.h" // zones_config_get_model/set_model/get_pid/set_pid/get_coupling/set_coupling_cell

// ---------------------------------------------------------------------
// Layer 2/3 -- batch fit, blend, guard, and apply -- called only from
// adaptive_tune_run_end(), i.e. only ever at a run boundary. This is what
// makes a mid-firing bump structurally impossible: there is no other call
// site that can reach zones_config_set_model()/set_pid() from this module.
// ---------------------------------------------------------------------

// Returns true only on the happy path where zones_config_set_model()/
// set_pid() actually ran -- i.e. Kp/Ki/Kd were just rewritten this run from
// the SIMC recompute. adaptive_tune_run_end() uses this to decide whether
// adaptive_tune_refine_ki_locked() may run this same run -- see that call site's own
// comment (D5).
//
// F3 (docs/audits/FLASH_WORKER_LOCK_INVERSION_AUDIT_2026-10-09.md): every
// zones_config_set_*() below ends in the zones NVS save, which dispatches onto
// the flash worker; the worker itself takes adaptive_tune_lock (UART AUTOTUNE
// ACCEPT -> adaptive_tune_clear_ki_baseline()). So the refine is split in
// three: plan_*_locked() (decisions only, adaptive_tune_lock held, NO zones
// setter), apply_*_plan() (the setters, adaptive_tune_lock NOT held) and
// commit_*_locked() (lock retaken, outcome-dependent state written).
// adaptive_tune_refine_zone_locked()/_coupled_locked() at the end of each
// section run all three back to back for callers that do not hold the lock
// across a flash-worker dispatch (host tests).
bool adaptive_tune_plan_zone_locked(uint8_t zi, uint8_t profile_id, adaptive_tune_zone_plan_t *plan)
{
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
    memset(plan, 0, sizeof(*plan));
    plan->profile_id = profile_id;
    plan->clear_gen = adaptive_tune_ki_clear_gen[zi];

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

    // docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md's
    // defect: the plausibility ratio test below used to read the LIVE
    // model_k_dc -- which THIS SAME FUNCTION is the only thing that ever
    // moves once a zone is opted in. That made "is this fit plausible" a
    // question checked against a reference that ratchets forward (or
    // backward) with every accepted run: a fit could be refused as >5x last
    // week's value while being, say, 40x the ORIGINAL autotune measurement,
    // as long as each intervening accepted run's own move stayed under 5x
    // its OWN immediately-prior value. No single run's guard was ever
    // wrong in isolation; composed across many accepted runs they placed no
    // ceiling on the total drift at all.
    //
    // Fix: anchor the ratio test (below) to autotune_baseline_k_dc -- the
    // K_dc the last FULL autotune Accept actually wrote
    // (autotune_engine_guard.c, via zones_config_set_autotune_baseline_
    // k_dc()), which THIS function never moves. The blend target and its
    // own per-run move cap are DELIBERATELY left reading the live k_dc,
    // unchanged -- see the comment just before the ratio check for why that
    // alone is enough to bound the whole module's lifetime range, without
    // sacrificing the intended multi-run convergence behavior a fixed,
    // repeated true gain is supposed to produce.
    //
    // A board that already has a model but predates this field (upgraded
    // from a pre-v26 blob, or simply never had this bootstrap run before)
    // reads 0 ("not recorded yet") -- bootstrapped here, once, from the live
    // model_k_dc, rather than either refusing outright (which would
    // permanently disable this zone until a fresh autotune) or silently
    // treating 0 as a real gain (which the ratio test below would then
    // reject every future fit against). A failed persist here is only
    // logged, not fatal to this run -- the in-RAM value below still anchors
    // this call correctly, and the next accepted run's zones_config_set_
    // model() write will not retry this bootstrap (has_applied semantics
    // aside, autotune_baseline_k_dc only needs to exist once), so a
    // transient NVS failure here is not silently invisible forever the way
    // it would be if nothing ever logged it.
    float baseline_k_dc;
    if (!zones_config_get_autotune_baseline_k_dc(zi, &baseline_k_dc) || !(baseline_k_dc > 0.0f)) {
        baseline_k_dc = k_dc;
        plan->bootstrap_baseline = true; // persisted by apply_zone_plan(), outside the lock
        plan->baseline_k_dc = baseline_k_dc;
    }

    // THE fix: the plausibility ratio test is anchored to baseline_k_dc (a
    // FIXED reference, never moved by this function), not to the live k_dc
    // this function itself writes. This is what turns the +-5x guard from a
    // per-run check into an actual LIFETIME ceiling -- proof below.
    if (k_fit > baseline_k_dc * ADAPTIVE_TUNE_MAX_JUMP_RATIO || k_fit < baseline_k_dc / ADAPTIVE_TUNE_MAX_JUMP_RATIO) {
        adaptive_tune_set_refusal(z, "fit %.4f is implausible against autotune baseline K %.4f (>%.0fx)", (double)k_fit,
                    (double)baseline_k_dc, (double)ADAPTIVE_TUNE_MAX_JUMP_RATIO);
        return false;
    }

    // Blend target and its own +-20% per-run move cap are DELIBERATELY still
    // relative to the LIVE k_dc, exactly as before this fix -- this is what
    // preserves the intended multi-run convergence behavior (H4(b), see
    // test_adaptive_tune_ki_verdict.c's test_ki_diagnosis_eventually_runs_
    // after_repeated_converging_refinements(): repeated runs against the
    // SAME true gain must keep closing the gap, asymptotically, not freeze
    // after the first accepted nudge). Only the PLAUSIBILITY reference
    // (immediately above) moved to the fixed baseline -- and that alone is
    // sufficient to cap this loop's lifetime range, by induction:
    //   - k_dc starts at baseline_k_dc (either a real prior autotune's value,
    //     or bootstrapped from it above), which is trivially inside
    //     [baseline/RATIO, baseline*RATIO].
    //   - Every k_fit this function ever accepts satisfies that same
    //     [baseline/RATIO, baseline*RATIO] bound (the ratio check just above,
    //     unconditionally, every run).
    //   - k_blended is a convex combination of k_dc and k_fit (ALPHA in
    //     (0,1)), and the +-20% clamp below only ever moves it TOWARD k_dc --
    //     both operations preserve membership in any interval that already
    //     contains both endpoints.
    // So if k_dc is in [baseline/RATIO, baseline*RATIO] before a run, it
    // still is after -- for every run, forever, regardless of how many
    // accepted refinements have run or how their individual fits trended.
    // This is what "the plausibility reference must be anchored to the
    // original autotune result" (docs/audits/adaptive_tune_vs_owner_
    // requirements_2026-09-11.md) actually buys: a hard, provable lifetime
    // envelope, without touching the per-run convergence dynamics at all.
    float k_blended = k_dc + ADAPTIVE_TUNE_BLEND_ALPHA * (k_fit - k_dc);
    float max_move = k_dc * ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE;
    if (k_blended > k_dc + max_move) k_blended = k_dc + max_move;
    if (k_blended < k_dc - max_move) k_blended = k_dc - max_move;
    if (!(k_blended > 0.0f)) {
        adaptive_tune_set_refusal(z, "blended gain %.4f is not positive", (double)k_blended);
        return false;
    }
    // Absolute physical ceiling -- see ADAPTIVE_TUNE_K_DC_ABS_MAX's own
    // comment. Independent of, and checked in addition to, the
    // baseline-relative envelope just above: that envelope bounds DRIFT from
    // this zone's own history, this bound catches a baseline itself that was
    // bootstrapped from (or an autotune Accept that wrote) an implausible
    // measurement in the first place.
    if (k_blended > ADAPTIVE_TUNE_K_DC_ABS_MAX) {
        adaptive_tune_set_refusal(z, "blended gain %.4f exceeds the absolute K_dc ceiling %.1f", (double)k_blended,
                    (double)ADAPTIVE_TUNE_K_DC_ABS_MAX);
        return false;
    }

    // F1: refuse rather than write a blend too small to be a material
    // change -- see ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC's own comment. This
    // check stays relative to the LIVE k_dc (not baseline_k_dc) deliberately
    // -- it is asking "would this write actually change anything from what
    // is live right now", a different question from the plausibility/blend
    // anchor above.
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

    // U1: snapshot exactly what is about to be overwritten -- the PRE-change
    // Kp/Ki/Kd (fetched fresh; nothing above this point has read them yet),
    // the prior K_dc/tau/dead_time already in hand as k_dc/tau_s/dead_time_s,
    // and the current ki_baseline state (still whatever it was before THIS
    // call's own re-latch below) -- see adaptive_tune_capture_revert_
    // locked()'s own comment for why this exact instant is the right one.
    float prior_kp = 0.0f, prior_ki = 0.0f, prior_kd = 0.0f;
    zones_config_get_pid(zi, &prior_kp, &prior_ki, &prior_kd); // best-effort -- an unreadable prior PID
                                                                // just leaves the revert snapshot at 0s,
                                                                // no worse than not having one
    const bool have_prior = zones_config_get_pid(zi, &prior_kp, &prior_ki, &prior_kd);
    adaptive_tune_capture_revert_locked(z, prior_kp, prior_ki, prior_kd, k_dc, tau_s, dead_time_s);
    plan->have_prior = have_prior;
    plan->prior_kp = prior_kp;
    plan->prior_ki = prior_ki;
    plan->prior_kd = prior_kd;

    plan->have_model = true;
    plan->k_dc = k_dc;
    plan->k_blended = k_blended;
    plan->tau_s = tau_s;
    plan->dead_time_s = dead_time_s;
    plan->kp = gains.kp;
    plan->ki = gains.ki;
    plan->kd = gains.kd;
    return true;
}

// No adaptive_tune_lock may be held (F3): the setters reach the flash worker.
void adaptive_tune_apply_zone_plan(uint8_t zi, adaptive_tune_zone_plan_t *plan)
{
    if (plan->bootstrap_baseline) {
        plan->bootstrap_ok = zones_config_set_autotune_baseline_k_dc(zi, plan->baseline_k_dc);
    }
    if (!plan->have_model) {
        return;
    }
    float live_kp, live_ki, live_kd;
    if (plan->have_prior && zones_config_get_pid(zi, &live_kp, &live_ki, &live_kd) &&
        (live_kp != plan->prior_kp || live_ki != plan->prior_ki || live_kd != plan->prior_kd)) {
        plan->stale = true; // another writer (autotune Accept) changed the gains after the plan
        return;
    }
    plan->model_ok = zones_config_set_model(zi, plan->k_blended, plan->tau_s, plan->dead_time_s);
    if (plan->model_ok) {
        plan->pid_ok = zones_config_set_pid(zi, plan->kp, plan->ki, plan->kd);
    }
}

// Returns true when the model AND PID writes both landed (same contract as the
// old adaptive_tune_refine_zone_locked()). adaptive_tune_lock held.
bool adaptive_tune_commit_zone_locked(uint8_t zi, const adaptive_tune_zone_plan_t *plan)
{
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
    float baseline_k_dc = plan->baseline_k_dc;
    float k_dc = plan->k_dc, k_blended = plan->k_blended, tau_s = plan->tau_s, dead_time_s = plan->dead_time_s;
    autotune_gains_t gains = {.kp = plan->kp, .ki = plan->ki, .kd = plan->kd};
    uint8_t profile_id = plan->profile_id;
    if (plan->bootstrap_baseline && !plan->bootstrap_ok) {
        ESP_LOGW(ADAPTIVE_TUNE_TAG,
                 "zone %u: could not persist bootstrapped autotune_baseline_k_dc=%.4f -- "
                 "using it in-RAM for this run only, will retry bootstrapping next run",
                 (unsigned)zi, (double)baseline_k_dc);
    }
    if (!plan->have_model) {
        return false; // plan refused; its refusal reason is already set
    }
    if (plan->stale) {
        adaptive_tune_set_refusal(z, "zone gains were changed by another writer during the run-end apply -- skipped");
        z->revert_available = false; // the snapshot taken at plan time describes a change that never landed
        return false;
    }
    if (!plan->model_ok) {
        adaptive_tune_set_refusal(z, "zones_config_set_model() rejected %.4f/%.1f/%.1f", (double)k_blended, (double)tau_s,
                    (double)dead_time_s);
        z->revert_available = false; // nothing was actually written -- do not offer a revert to a "before"
                                      // that never became a real "after"
        return false;
    }
    if (!plan->pid_ok) {
        adaptive_tune_set_refusal(z, "zones_config_set_pid() rejected %.4f/%.4f/%.4f", (double)gains.kp, (double)gains.ki,
                    (double)gains.kd);
        // The model write above DID land, even though the PID write just
        // failed -- z->revert_available stays true so a revert can still
        // undo the half-applied model change; its captured prior_kp/ki/kd
        // are still correct for that (the live PID triple never actually
        // changed on this failed path).
        return false;
    }

    if (plan->have_prior) {
        float live_kp, live_ki, live_kd;
        if (zones_config_get_pid(zi, &live_kp, &live_ki, &live_kd) &&
            (live_kp != gains.kp || live_ki != gains.ki || live_kd != gains.kd)) {
            // A writer landed after our set_pid: our gains are no longer live, so recording has_applied
            // and a revert snapshot would later undo THEIR gains.
            adaptive_tune_set_refusal(z, "zone gains were changed by another writer right after the run-end apply");
            z->revert_available = false;
            return false;
        }
    }

    z->last_refusal_reason[0] = '\0';
    z->has_applied = true;
    z->prior_k_dc = k_dc;
    z->applied_k_dc = k_blended;
    z->last_delta_pct = (k_dc > 0.0f) ? ((k_blended - k_dc) / k_dc) * 100.0f : 0.0f;
    z->last_applied_profile_id = profile_id;
    z->last_applied_unix_s = (uint32_t)time(NULL);

    // Q4: re-latch the Ki-diagnosis baseline (adaptive_tune_ki.c) to the Ki
    // this SIMC recompute just wrote -- see adaptive_tune_refine_ki_locked()'s own
    // comment on ki_baseline for the full "which layer is the more
    // authoritative reference" reasoning. This is the ONLY setter this
    // module writes gains.ki through, so this is the one place a fresh SIMC
    // Ki exists to latch from. adaptive_tune_run_end() (adaptive_tune.c)
    // detects this write and dispatches the NVS persist the same way it
    // already does for adaptive_tune_ki.c's own latch -- see that function's
    // baseline_newly_latched comment.
    // F3: the lock was dropped for the setters; an UART Accept that cleared
    // this zone's baseline in that window also rewrote the live PID, so the SIMC
    // Ki planned above is stale -- do not re-latch it.
    if (adaptive_tune_ki_clear_gen[zi] == plan->clear_gen) {
        z->ki_baseline = gains.ki;
        z->ki_baseline_valid = true;
    }

    ESP_LOGI(ADAPTIVE_TUNE_TAG, "zone %u: K_dc %.4f -> %.4f (%.1f%%) from %u observations, profile %u", (unsigned)zi,
             (double)k_dc, (double)k_blended, (double)z->last_delta_pct, (unsigned)z->ring_count,
             (unsigned)profile_id);
    return true;
}

// Plan + apply + commit in one call: ONLY for callers that do not hold
// adaptive_tune_lock across the zones setters (adaptive_tune_run_end() must
// not use this -- it drives the three phases itself).
bool adaptive_tune_refine_zone_locked(uint8_t zi, uint8_t profile_id)
{
    adaptive_tune_zone_plan_t plan;
    bool planned = adaptive_tune_plan_zone_locked(zi, profile_id, &plan);
    if (planned || plan.bootstrap_baseline) {
        adaptive_tune_apply_zone_plan(zi, &plan);
    }
    return adaptive_tune_commit_zone_locked(zi, &plan);
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
// Physical-sanity gate on a FITTED coupled matrix (out_C[affected][stepped],
// same orientation as storage -- NOT /api/autotune/matrix's transposed wire
// form, so no transpose is needed or wanted here), run AFTER the pivot-floor
// conditioning check in adaptive_tune_coupled_fit() passes and BEFORE any
// blending or write. Mirrors tools/PcTools/src/kilnctrl/coupled_ident.py's
// matrix_plausibility() deliberately, so an operator reading a firmware
// refusal and an offline analysis of the same captures see the same
// verdict -- but re-derived here, not copied, because each criterion has to
// be justified on its own physical grounds:
//
//   1. Every entry must be non-negative. This is a statement about the
//      KILN, not the fit: more duty on any zone can only add heat to a
//      neighbor through conduction/radiation, never remove it, so a fitted
//      coupling_coeff[i][j] < 0 cannot be "a small unimportant term" --
//      it is proof the fit is describing measurement noise, not a real
//      thermal path. (This is a real, not hypothetical, finding: one 3-
///     observation/3-unknown capture in this codebase's plant_sim fixtures
//      produced a perfect-residual interpolation with three negative
//      entries.)
//   2. Each row's own-zone (diagonal) coefficient must be its row's largest
//      entry. Physically, a zone's own heater sits closer to its own
//      thermocouple than any other zone's heater does, in every geometry
//      this kiln has -- self-coupling dominating cross-coupling is the
//      expected shape of ANY plausible row, not an assumption specific to
//      the current bench-measured matrix. A row where a neighbor's duty
//      outweighs the zone's own is either a wiring/labeling fault or, as
//      measured on the 26-observation plant_sim fixture set (condition
//      number 44.3 -- comfortably under COUPLING_SOLVE_PIVOT_REL_EPS's
//      ~1e4 admission bound, so the conditioning gate alone says nothing
//      is wrong here), two rows collapsing their own-duty coefficient
//      toward zero (0.8 against a real ~32) while dumping the sensitivity
//      onto an off-diagonal (38) instead -- the near-parallel, same-
//      setpoint duty vectors every ordinary firing produces leave enough
//      freedom for quantization noise to swap which column "explains" a
//      row, and conditioning alone cannot see that swap happen.
//
// Both are necessary, neither is sufficient on its own to prove a fit is
// good -- together they catch the two concrete failure shapes actually
// observed on real captures, without claiming to be a complete physical
// model of the kiln.
static adaptive_tune_coupled_result_t adaptive_tune_matrix_plausible(
    const float C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT], uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            if (C[i][j] < 0.0f) {
                return ADAPTIVE_TUNE_COUPLED_IMPLAUSIBLE_MATRIX;
            }
        }
    }
    for (uint8_t i = 0; i < n; i++) {
        float own = C[i][i];
        for (uint8_t j = 0; j < n; j++) {
            if (j == i) continue;
            if (own < C[i][j]) {
                return ADAPTIVE_TUNE_COUPLED_IMPLAUSIBLE_MATRIX;
            }
        }
    }
    return ADAPTIVE_TUNE_COUPLED_OK;
}

// RTC_NOINIT_ATTR: deliberately NOT zeroed by the startup code, so it
// survives a software reset (panic, watchdog, esp_restart) -- same pattern
// and same reasoning as boot_guard.c's s_bg_rtc. Garbage after a power-on
// reset is rejected by the magic check in adaptive_tune_coupled_breadcrumb_
// get() below. See adaptive_tune_internal.h's own comment on this type for
// why this exists.
RTC_NOINIT_ATTR static adaptive_tune_coupled_breadcrumb_t s_coupled_bc;

void adaptive_tune_coupled_breadcrumb_mark(adaptive_tune_coupled_breadcrumb_stage_t stage, uint8_t zone_index,
                                            uint32_t joint_observations, uint32_t solve_row)
{
    s_coupled_bc.magic = ADAPTIVE_TUNE_COUPLED_BC_MAGIC;
    s_coupled_bc.seq++;
    s_coupled_bc.stage = (uint32_t)stage;
    s_coupled_bc.zone_index = zone_index;
    s_coupled_bc.joint_observations = joint_observations;
    s_coupled_bc.solve_row = solve_row;
    s_coupled_bc.stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

bool adaptive_tune_coupled_breadcrumb_get(adaptive_tune_coupled_breadcrumb_t *out)
{
    if (!out) {
        return false;
    }
    if (s_coupled_bc.magic != ADAPTIVE_TUNE_COUPLED_BC_MAGIC) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = s_coupled_bc;
    return true;
}

bool adaptive_tune_coupled_breadcrumb_is_mid_solve(const adaptive_tune_coupled_breadcrumb_t *bc)
{
    if (!bc || bc->magic != ADAPTIVE_TUNE_COUPLED_BC_MAGIC) {
        return false;
    }
    return bc->stage != (uint32_t)ADAPTIVE_TUNE_COUPLED_BC_IDLE &&
           bc->stage != (uint32_t)ADAPTIVE_TUNE_COUPLED_BC_RETURNED;
}

adaptive_tune_coupled_result_t adaptive_tune_coupled_fit(
    const float duty_obs[][MAX31856_CHANNEL_COUNT], const float rise_obs[][MAX31856_CHANNEL_COUNT], uint32_t m,
    uint8_t n, float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT])
{
    if (!duty_obs || !rise_obs || !out_C || n == 0 || n > MAX31856_CHANNEL_COUNT) {
        return ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS;
    }
    // m >= n + ADAPTIVE_TUNE_COUPLED_OBS_MARGIN (not just m >= n) is kept
    // deliberately, not raised further here: the real defect this gate
    // exists alongside was measured on a 26-observation fixture set (n=3,
    // margin would need to reach ~23 to have blocked it), so observation
    // COUNT was never what let the bad matrix through -- the duty vectors'
    // DIRECTION (near-parallel, same-setpoint) was. Raising the margin
    // further would refuse good data without addressing the actual failure
    // mode; adaptive_tune_matrix_plausible() below is what catches it.
    // margin=2 does still buy something real, independent of that: it
    // guarantees m > n, i.e. at least 2 degrees of freedom in the least-
    // squares solve, so this path can never land on the m==n exactly-
    // determined case, where the residual is identically zero and a
    // "perfect fit" is really zero evidence of fit quality rather than a
    // good sign.
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
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_BEFORE_GAUSS_SOLVE, 0xFF, m, i);
        coupling_solve_reason_t reason = zone_coupling_gauss_solve_partial_pivot_vec(n, AtA, AtR, x);
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_AFTER_GAUSS_SOLVE, 0xFF, m, i);
        if (reason != COUPLING_SOLVE_OK) {
            return ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED;
        }
        for (uint8_t j = 0; j < n; j++) {
            out_C[i][j] = x[j];
        }
    }

    // Physical plausibility, applied to the WHOLE fitted matrix, before this
    // function reports success -- see adaptive_tune_matrix_plausible()'s own
    // comment just above for why this is a separate gate from the
    // conditioning check above, not folded into it.
    adaptive_tune_coupled_result_t plausibility = adaptive_tune_matrix_plausible(out_C, n);
    if (plausibility != ADAPTIVE_TUNE_COUPLED_OK) {
        return plausibility;
    }
    return ADAPTIVE_TUNE_COUPLED_OK;
}

void adaptive_tune_plan_coupled_locked(uint8_t zi, adaptive_tune_coupled_plan_t *plan)
{
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
    memset(plan, 0, sizeof(*plan));
    z->coupled_attempted = true;
    z->coupled_applied = false;
    z->coupled_cells_changed = 0;
    z->joint_observations = adaptive_tune_joint_ring_count;

    const uint8_t n = MAX31856_CHANNEL_COUNT;
    uint32_t min_obs = (uint32_t)n + ADAPTIVE_TUNE_COUPLED_OBS_MARGIN;
    adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_ENTERED, zi, adaptive_tune_joint_ring_count, 0);
    if (adaptive_tune_joint_ring_count < min_obs) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "only %u/%u joint dwell observations for coupled solve", (unsigned)adaptive_tune_joint_ring_count,
                   (unsigned)min_obs);
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
        return;
    }

    // HEAP, not stack (2026-10-09): ADAPTIVE_TUNE_JOINT_RING_CAPACITY(24) *
    // MAX31856_CHANNEL_COUNT(3) * 4 bytes * 2 arrays = 576 bytes. As stack
    // locals they made this frame 704 B, and on the profile executor's
    // run-end path (executor_task_entry -> adaptive_tune_run_end -> here ->
    // adaptive_tune_coupled_fit -> zone_coupling_gauss_solve_partial_pivot_vec)
    // that reached 2080 B against check_executor_task_stack_budget's 1936 B
    // ceiling. Freed on every return path below.
    typedef float joint_obs_t[ADAPTIVE_TUNE_JOINT_RING_CAPACITY][MAX31856_CHANNEL_COUNT];
    struct {
        joint_obs_t duty;
        joint_obs_t rise;
    } *obs = persist_scratch_alloc(sizeof(*obs)); /* PSRAM first; OOM refusal below unchanged */
    if (!obs) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "out of memory for the coupled solve's observation buffers");
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
        return;
    }
    for (uint32_t k = 0; k < adaptive_tune_joint_ring_count; k++) {
        uint32_t idx = (adaptive_tune_joint_ring_head + k) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
        memcpy(obs->duty[k], adaptive_tune_joint_ring[idx].duty, sizeof(obs->duty[k]));
        memcpy(obs->rise[k], adaptive_tune_joint_ring[idx].rise_c, sizeof(obs->rise[k]));
    }
    adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RING_COPIED, zi, adaptive_tune_joint_ring_count, 0);

    float C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_BEFORE_FIT, zi, adaptive_tune_joint_ring_count, 0);
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(obs->duty, obs->rise, adaptive_tune_joint_ring_count, n, C);
    free(obs);
    obs = NULL;
    adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_AFTER_FIT, zi, adaptive_tune_joint_ring_count, 0);
    if (r == ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "joint observation set degenerate for a determined solve");
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
        return;
    }
    if (r == ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "joint duty matrix ill-conditioned (cond above ~1e4, zone_coupling_solve.h's own pivot floor)");
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
        return;
    }
    // Distinct reason string from ILL_CONDITIONED above on purpose -- an
    // operator needs to be able to tell "not determinable from this data"
    // (more/better observations would help) apart from "determinable, but
    // the answer is not physically possible" (this data's duty vectors are
    // too collinear for THIS solve to trust, no matter how low its
    // condition number looks -- see adaptive_tune_matrix_plausible()'s own
    // comment above this function's definition).
    if (r == ADAPTIVE_TUNE_COUPLED_IMPLAUSIBLE_MATRIX) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "fitted coupling matrix is physically implausible (negative coefficient or a row not "
                   "dominated by its own zone) -- determinable by conditioning alone but not trustworthy");
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
        return;
    }

    float prior_row[MAX31856_CHANNEL_COUNT];
    if (!zones_config_get_coupling(zi, prior_row)) {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "no existing coupling row to refine");
        adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
        return;
    }
    float tau_row[MAX31856_CHANNEL_COUNT], dead_row[MAX31856_CHANNEL_COUNT];
    if (!zones_config_get_coupling_tau(zi, tau_row)) memset(tau_row, 0, sizeof(tau_row));
    if (!zones_config_get_coupling_dead_time(zi, dead_row)) memset(dead_row, 0, sizeof(dead_row));

    for (uint8_t j = 0; j < n; j++) {
        if (j == zi) {
            continue; // diagonal (this zone's own gain) stays owned by the existing per-zone K_dc
                      // path (adaptive_tune_refine_zone_locked()) -- not duplicated here, see this function's
                      // header comment.
        }
        // docs/SPARE_RELAY_ONOFF_PLAN.md sec 10: a monitor-only column is
        // masked by the same getter for the same reason, so it is skipped
        // for the same reason as an on/off column below.
        if (zone_is_on_off(j) || zone_is_monitor_only(j)) {
            // A4 review follow-up B (2026-09-28): zones_config_get_coupling()
            // (the getter prior_row was just filled from, above) always masks
            // an on/off zone's COLUMN to 0.0f -- "this zone injects no heat
            // into anyone" (docs/ON_OFF_ZONE.md sec 1). That masked 0.0
            // is not "no prior", it's "control ignores this cell entirely",
            // and this loop's own near-zero branch below treats prior==0 as
            // "no confident prior yet" and blends 0.15*fit through
            // zones_config_set_coupling_cell(), permanently overwriting
            // whatever real coefficient is actually stored in flash for this
            // column (found on the bench: an on/off zone 2 retyped from a
            // heater still had real z0[2]/z1[2] cells, and an adaptive update
            // after retyping stomped them to ~0.15*fit).
            //
            // Fix is to skip the column outright rather than read
            // zones_config_get_coupling_raw() for the prior: an on/off zone's
            // outgoing coupling is inert everywhere control reads it (the
            // masking getter is a live-control-loop guard, not just a backup
            // quirk), so a fitted coefficient into that column would never be
            // read back except by another adaptive pass computing yet another
            // fit against it -- there is no reader this write could ever
            // usefully feed. Skipping preserves whatever real, pre-retype
            // value is already on disk (in case the zone is ever retyped back
            // to a heater) instead of clobbering it with a coefficient fit
            // while the zone was on/off.
            continue;
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

        plan->cells[plan->count].j = j;
        plan->cells[plan->count].blended = blended;
        plan->cells[plan->count].tau = tau_row[j];
        plan->cells[plan->count].dead = dead_row[j];
        plan->count++;
    }
    plan->reached_apply = true;
}

// No adaptive_tune_lock may be held (F3).
void adaptive_tune_apply_coupled_plan(uint8_t zi, adaptive_tune_coupled_plan_t *plan)
{
    for (uint8_t c = 0; c < plan->count; c++) {
        plan->cells[c].ok = zones_config_set_coupling_cell(zi, plan->cells[c].j, plan->cells[c].blended,
                                                           plan->cells[c].tau, plan->cells[c].dead);
    }
}

void adaptive_tune_commit_coupled_locked(uint8_t zi, const adaptive_tune_coupled_plan_t *plan)
{
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
    if (!plan->reached_apply) {
        return; // plan already recorded its refusal and breadcrumb
    }
    uint8_t changed = 0;
    for (uint8_t c = 0; c < plan->count; c++) {
        if (plan->cells[c].ok) {
            changed++;
        }
    }
    z->coupled_cells_changed = changed;
    if (changed > 0) {
        z->coupled_applied = true;
        z->coupled_refusal_reason[0] = '\0';
        ESP_LOGI(ADAPTIVE_TUNE_TAG, "zone %u: coupled solve refined %u coupling cell(s) from %u joint observations",
                 (unsigned)zi, (unsigned)changed, (unsigned)adaptive_tune_joint_ring_count);
    } else {
        adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "coupled solve succeeded but every off-diagonal cell was implausible or rejected");
    }
    adaptive_tune_coupled_breadcrumb_mark(ADAPTIVE_TUNE_COUPLED_BC_RETURNED, zi, adaptive_tune_joint_ring_count, 0);
}

// Plan + apply + commit in one call -- NOT for adaptive_tune_run_end() (see
// adaptive_tune_refine_zone_locked()).
void adaptive_tune_refine_coupled_locked(uint8_t zi)
{
    adaptive_tune_coupled_plan_t plan;
    adaptive_tune_plan_coupled_locked(zi, &plan);
    adaptive_tune_apply_coupled_plan(zi, &plan);
    adaptive_tune_commit_coupled_locked(zi, &plan);
}
