// zone_coupling_solve -- pure cross-zone coupling/feedforward math split out
// of profile_executor.c, following the same rationale as dashboard_json.c and
// zones_config_json.c: this is genuinely self-contained numeric/logic code
// (Gaussian elimination, a low-pass filter tick, a neighbour-qualification
// predicate, the coupled steady-state hold solve) with no FreeRTOS lock, no
// hardware access, and no dependency on profile_executor.c's big
// s_exec.lock-guarded global -- but it used to be defined INSIDE
// profile_executor.c, so proving a change to it correct meant recompiling
// (and, until test_profile_executor_prestart.c #includes the .c file
// directly, indirectly depending on) the whole 4800+ line executor file.
//
// The one wrinkle: the original functions took a `zone_runtime_t *`, which is
// a struct PRIVATE to profile_executor.c (not declared in profile_executor.h)
// and drags in heater_output.h/thermal_guard.h/pid.h field types this module
// has no business knowing about. Rather than hoist that struct into a shared
// header (or, worse, duplicate its ~30 fields here and hope the two copies
// never drift), every function below takes exactly the scalar fields it
// reads, or the small purpose-built zone_coupling_neighbor_t array below --
// nothing more. profile_executor.c builds that array from s_exec.zones[]
// (self included, at zones[zi]) once per call and passes it in; the cache row
// and membership-signature slot the coupled solve needs are likewise passed
// in as pointers into arrays profile_executor.c still owns and allocates
// (s_coupling_hold_cache[]/s_coupling_prev_membership_sig[]), so this module
// carries no hidden mutable state of its own and the caching/damping
// behaviour is unchanged, byte-for-byte, from the pre-split code.
//
// Matrix orientation: unchanged from the original. zones_config_get_coupling()
// (zones_http.h) returns zone_cfg_t::coupling_coeff[] as ROW = the zone being
// asked about (the AFFECTED zone), COLUMN = the neighbour (the STEPPED zone)
// -- see zones_http.h's own doc comment above that function. Both
// zone_coupling_count_qualifying_neighbors() and zone_coupling_solve_hold()
// call zones_config_get_coupling(s, coupling_row) and index coupling_row[t]
// exactly as profile_executor.c always did; nothing here transposes it.
#ifndef ZONE_COUPLING_SOLVE_H
#define ZONE_COUPLING_SOLVE_H

#include <stdbool.h>
#include <stdint.h>

#include "MAX31856.h" /* MAX31856_CHANNEL_COUNT */
#include "zones_config_accessors.h" /* zone_control_mode_t, zones_config_get_coupling() */

#ifdef __cplusplus
extern "C" {
#endif

/* One entry per zone (indexed 0..zone_count-1, self included at its own
 * index) -- the only per-zone facts zone_coupling_count_qualifying_neighbors()
 * and zone_coupling_solve_hold() need. Built by profile_executor.c from
 * s_exec.zones[] via zone_coupling_qualifies_as_neighbor() below, once per
 * call, so the two never see a stale or independently-derived answer. */
typedef struct {
    bool  qualifies; /* zone_coupling_qualifies_as_neighbor()'s verdict for this zone */
    float ff_k_dc;   /* this zone's own steady-state gain, degC per unit duty */
    float ff_tau_s;  /* this zone's own plant time constant, seconds -- only read by
                      * zone_coupling_solve_climb() (the per-row RHS rate*tau_i is NOT
                      * uniform across members the way the hold term's dT is, since tau
                      * differs per zone -- see that function's doc comment). Unused by
                      * the hold path. */
} zone_coupling_neighbor_t;

/* True iff zone zn is genuinely under closed-loop control on the shared
 * setpoint right now, i.e. its temperature error is attributable to ITS OWN
 * heater the way the cross-zone coupling derivation (see
 * zone_coupling_solve_hold()'s doc comment) requires. Used both to decide
 * whether a zone contributes as a NEIGHBOUR to another zone's system and
 * (self-applied, zi's own fields) whether zi's system is solved at all --
 * profile_executor.c calls this once per zone per tick to build the
 * zone_coupling_neighbor_t array above, so the two questions can never drift
 * apart the way two independently-written gates could.
 *
 * control_mode: only the PID family actually closes the loop on setpoint
 * every tick; BANGBANG's on/off band is not the linear response this term's
 * derivation assumes, and OFF drives nothing at all.
 * faulted: a per-zone guard trip means duty is being forced off/limited for a
 * reason unrelated to normal setpoint tracking.
 * heat_blocked: relay authority's answer as of the last tick that wanted
 * heat -- a zone whose relay authority is refusing ON is not being driven
 * either, even though nothing else here would notice. */
bool zone_coupling_qualifies_as_neighbor(bool active, bool actual_valid, float actual_c, bool ff_enabled,
                                         float ff_k_dc, zone_control_mode_t control_mode, bool faulted,
                                         bool heat_blocked);

/* How many of zone zi's neighbours (zones[] index != zi) actually contribute
 * to its coupling term right now -- i.e. have a nonzero/finite coefficient in
 * zi's coupling row AND qualify per zone_coupling_qualifies_as_neighbor(). */
uint8_t zone_coupling_count_qualifying_neighbors(uint8_t zi, const zone_coupling_neighbor_t *zones,
                                                 uint8_t zone_count);

/* Advances *filtered_c by one tick -- the cross-zone coupling term's
 * neighbour-side low-pass. Must be called exactly once per control tick, from
 * the per-channel reading loop right after actual_c/actual_valid are set for
 * that tick -- NOT from inside the feedforward computation, even though that
 * is what reads the result (see profile_executor.c's call site for why:
 * seed_bumpless_with_ff() calls the feedforward computation a second time for
 * the "same" tick, and advancing the filter there would desynchronize the
 * seed from the tick it is seeding for).
 *
 * Same tau/alpha formula as pid.c's D-term low-pass, deliberately: reuses
 * this zone's own d_filter_tau_s -- the same value pid.c already trusts as
 * "slow enough to be signal, not noise" for this exact sensor.
 *
 * Frozen (not reset to 0 or NAN), not reset, on an invalid reading -- same
 * "hold last value" freeze pid.c's d_filtered gets outside pid_range_c. A
 * neighbour with a bad reading THIS instant is excluded from the coupling sum
 * entirely by zone_coupling_qualifies_as_neighbor() (actual_valid is one of
 * its checks), so a stale filtered value sitting unused does no harm. */
void zone_coupling_filter_tick(float *filtered_c, bool *filter_init, float actual_c, bool actual_valid_now,
                               float d_filter_tau_s, float dt_s);

/* Tightened from 1e-6 (Opus review, blocker 1): float32 machine epsilon is
 * ~1.19e-7 (~7.2 decimal digits), so a 1e-6 RELATIVE floor admits condition
 * numbers up to ~1e6 -- at that boundary the solve has already lost ~6 of
 * its ~7 digits and the returned duties can be pure rounding noise, yet they
 * would be marked COUPLING_SOLVE_OK (not flagged infeasible unless they
 * happen to also land outside [0,1]) and go straight to heater feedforward.
 * 1e-4 admits condition numbers up to ~1e4 (~3 surviving digits) -- still
 * generous against every physically plausible coupling matrix: the real
 * bench-measured matrix (PID_EXPANSION_PLAN.md Phase 0) has cond_inf = 2.94,
 * and diagonal dominance (diag 22-32 vs off-diag 2-12 -- a heater's own zone
 * always couples to itself harder than to its neighbours) keeps any
 * genuinely-measured matrix in the single digits. Nothing legitimate is
 * refused; see test_hold_pivot_floor_just_{inside,outside}_condition_number_*
 * for the boundary pinned exactly at cond ~1e4. DO NOT change this value as
 * part of any refactor -- it is a tuned physical threshold, not a placeholder. */
#define COUPLING_SOLVE_PIVOT_REL_EPS 1e-4f
#define COUPLING_SOLVE_PIVOT_ABS_EPS 1e-9f

/* Opus review, blocker 3: used_matrix collapsed four independently-testable
 * causes into one boolean; each fallback reason is now its own enum value. */
typedef enum {
    COUPLING_SOLVE_OK = 0,
    COUPLING_SOLVE_FALLBACK_OUT_OF_RANGE,   /* zi >= MAX31856_CHANNEL_COUNT, or a non-finite
                                             * setpoint/ambient input */
    COUPLING_SOLVE_FALLBACK_UNQUALIFIED,    /* zi itself fails zone_coupling_qualifies_as_neighbor() */
    COUPLING_SOLVE_FALLBACK_NO_NEIGHBORS,   /* zi qualifies but has none -- the 1x1 system IS the
                                             * legacy diagonal formula, not an approximation of it */
    COUPLING_SOLVE_FALLBACK_SINGULAR,       /* gauss_solve_partial_pivot(): a pivot fell below the
                                             * conditioning floor -- singular/ill-conditioned data */
    COUPLING_SOLVE_FALLBACK_NONFINITE,      /* gauss_solve_partial_pivot() produced a non-finite
                                             * component despite every pivot clearing the floor */
    COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE, /* the matrix would have been assembled from two
                                             * different identification experiments -- a member
                                             * column carries measured off-diagonals but no
                                             * measured coupling_diag_k_dc (or the reverse), or
                                             * use_measured_diag_k_dc is false while measured
                                             * off-diagonals exist. See
                                             * coupling_column_provenance_ok() in
                                             * zone_coupling_solve.c and
                                             * docs/audits/dc_gain_factor_of_ten_2026-09-09.md
                                             * sec 4. Appended at the END of this enum
                                             * deliberately: zone_runtime_t::ff_hold_reason is a
                                             * uint8_t copy of it that reaches the dashboard JSON
                                             * as a number, so renumbering an existing value
                                             * would silently relabel historical logs. */
} coupling_solve_reason_t;

typedef struct {
    bool    have;
    uint8_t n;
    uint8_t members[MAX31856_CHANNEL_COUNT];
    float   G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    float   b;
    float   u[MAX31856_CHANNEL_COUNT];
    coupling_solve_reason_t reason;
    bool    infeasible;
} zone_coupling_hold_cache_t;

/* Gaussian elimination with partial pivoting on the n x n system G*u = b (b
 * constant across every row -- see zone_coupling_solve_hold()'s doc comment
 * for the dT_i derivation). Detects a singular/ill-conditioned system by
 * watching pivot magnitudes during elimination -- relative to the largest
 * entry anywhere in the ORIGINAL matrix, so a uniformly tiny but
 * well-conditioned matrix is not mistaken for a singular one -- rather than
 * forming a determinant or an inverse separately. Pure stack float math (an
 * (n+1)-augmented MAX31856_CHANNEL_COUNT-sized matrix, trivially small), no
 * dynamic allocation. out_u[] is left untouched unless COUPLING_SOLVE_OK is
 * returned -- callers must not read it otherwise.
 *
 * Only THREE outcomes are reachable, and each is independently falsifiable
 * (Opus review, blocker 3) -- proven by disabling each in turn and watching
 * the corresponding test fail:
 *   - COUPLING_SOLVE_FALLBACK_SINGULAR: the pivot-floor check. The
 *     load-bearing conditioning guard -- see COUPLING_SOLVE_PIVOT_REL_EPS.
 *   - COUPLING_SOLVE_FALLBACK_NONFINITE: the back-substitution isfinite(v)
 *     check. Reachable despite every pivot clearing the floor: `sum`
 *     accumulates M[i][j]*out_u[j] terms, and while each individual M[i][j]
 *     is bounded by `scale` (the largest ORIGINAL entry), out_u[j] itself is
 *     NOT floor-bounded -- a huge-but-technically-conditioned matrix can
 *     still overflow `sum` to +-Inf here.
 *   - COUPLING_SOLVE_OK: every pivot cleared the floor and every division
 *     produced a finite value.
 *
 * TWO other checks that used to exist here were deleted, not kept as
 * unfalsifiable padding, because they are provably dead given the two
 * guards above -- see zone_coupling_solve.c's copy of this function for the
 * full proof (unchanged from the pre-split version, just relocated). */
coupling_solve_reason_t zone_coupling_gauss_solve_partial_pivot(uint8_t n,
                                                                 const float G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT],
                                                                 float b_const, float out_u[MAX31856_CHANNEL_COUNT]);

/* Same Gaussian elimination as zone_coupling_gauss_solve_partial_pivot() --
 * identical pivoting, conditioning floor and reachable-outcome set -- except
 * the right-hand side is PER ROW rather than one scalar shared by every row.
 * zone_coupling_gauss_solve_partial_pivot() is now a thin wrapper over this
 * one (fills an n-long array with b_const and calls through), so the two
 * never drift: there is exactly one elimination implementation, not two
 * copies that happen to agree today. b[i] corresponds to row i, i.e. the same
 * row zone_coupling_solve_hold()/zone_coupling_solve_climb() populate as
 * G[i][*] for `members[i]` -- callers with a uniform RHS (the hold term) can
 * keep calling the scalar form above instead of filling an array by hand. */
coupling_solve_reason_t zone_coupling_gauss_solve_partial_pivot_vec(uint8_t n,
                                                                     const float G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT],
                                                                     const float b[MAX31856_CHANNEL_COUNT],
                                                                     float out_u[MAX31856_CHANNEL_COUNT]);

/* Builds the reduced coupled system for zone zi and returns its hold duty, via
 * *cache_row (profile_executor.c's s_coupling_hold_cache[zi]) and
 * *prev_membership_sig (profile_executor.c's s_coupling_prev_membership_sig[zi]) --
 * both owned and allocated by the caller, one MAX31856_CHANNEL_COUNT-sized
 * array each, passed in by reference to this zi's own slot so this module
 * carries no static/hidden state of its own while the cache-hit and
 * membership-damping behaviour stays exactly what it was before the split.
 *
 * `z_qualifies`/`z_ff_k_dc` are zi's OWN fields, evaluated by the caller from
 * its own zone_runtime_t* (`z`) rather than read out of the `zones` array at
 * index zi -- the original solve_hold_for_zone() took `z` as a parameter
 * SEPARATE from s_exec.zones[] specifically so its host tests could drive a
 * zone's own state without populating s_exec.zones[] for it (see the
 * original's doc comment, carried into this header's own notes above), and
 * computing diagonal_hold from z_ff_k_dc/coupling_diagonal_k_dc(zi, ...)
 * (never `zones[zi]`) -- before zi is even range-checked -- means an
 * out-of-range zi can never index `zones[]` out of bounds while computing
 * it (coupling_diagonal_k_dc()'s own call into
 * zones_config_get_coupling_diag_k_dc() bound-checks zi against the config
 * layer's zone count independently and degrades to z_ff_k_dc on failure, so
 * an out-of-range zi is still safe here). `zones` supplies every OTHER
 * (neighbour) zone's fields; zones[zi] itself is never read.
 *
 * *out_reason is COUPLING_SOLVE_OK exactly when the return value came from a
 * genuine solve; any other value means the return is the untouched legacy
 * diagonal formula (setpoint_c-ambient_c)/z_ff_k_dc -- see
 * coupling_solve_reason_t's own doc comment for what each fallback reason
 * means. *out_used_matrix is `*out_reason == COUPLING_SOLVE_OK`, kept as a
 * separate bool purely so callers that only care about the yes/no question do
 * not have to compare an enum.
 *
 * *out_infeasible is only ever true alongside a genuine solve, meaning the
 * solve needed to clamp some zone's duty DOWN to 1.0 -- more heat than that
 * zone can physically deliver was requested. A negative raw component is NOT
 * infeasible and is left un-clamped here (Opus review, "b < 0 case"): needing
 * less than zero duty is always trivially achievable (the heater simply does
 * not fire) and is exactly what the legacy per-zone formula already returns
 * unclamped on a cooling segment (setpoint below ambient), to be combined
 * with the (also possibly negative) climb term and clamped only in the
 * caller's own final [0,1] sum -- not here.
 *
 * *out_membership_changed is true when the SET of zones in this system (or
 * whether zi qualifies at all) differs from the last call for this zi. Callers
 * that care about bump-transfer (pid_family_zone_tick()) must re-seed on this
 * edge; this function only detects and reports it.
 *
 * `use_measured_diag_k_dc` (PID_EXPANSION_PLAN.md sec 3.2, "STORAGE LANDED
 * 2026-09-02f" / "the solver switch itself"): when true, each member row's
 * G[row][row] tries `zones_config_get_coupling_diag_k_dc(member, &v)` first
 * and uses it if that call reports success AND `v` is finite and > 0.0f --
 * the SAME guarded-fallback shape this file's own diagonal_hold/
 * diagonal_climb use for `ff_k_dc` (0.0f/not-finite/not-measured falls
 * through). Any other outcome (getter reports false, or the stored value is
 * not usable) falls back to `ff_k_dc` exactly as when this flag is false.
 * False reproduces every caller's pre-existing behaviour bit-for-bit (the
 * hybrid this codebase has run since 2026-09-02d): a caller must opt in per
 * call, there is no compiled-in default that changes shipped behaviour by
 * itself. See the header's own analysis for why this is not (yet) the
 * shipped default: `coupling_diag_k_dc` has no autotune writer on this
 * board today, only a PC-side preset or hand-set value.
 *
 * CLOSED, 2026-09-03: the uncoupled 1x1 fallback's own diagonal choice.
 * `diagonal_hold` (this function) and `diagonal_climb`
 * (zone_coupling_solve_climb()) now compute their diagonal via
 * `coupling_diagonal_k_dc(zi, z_ff_k_dc, use_measured_diag_k_dc)` -- the
 * SAME helper the n>1 matrix path's G[row][row] uses for row==zi -- instead
 * of reading z_ff_k_dc directly. This removes the seam sec 3.2's
 * 2026-09-02e pass sized rather than closed: previously the diagonal source
 * SWITCHED at every membership edge (1x1 always `z_ff_k_dc`, n>1
 * `coupling_diag_k_dc`-when-measured); now both paths agree on zi's own
 * diagonal by construction, for every value of the flag, because they call
 * the identical function. Every solver-failure fallback
 * (COUPLING_SOLVE_FALLBACK_SINGULAR / _NONFINITE / _OUT_OF_RANGE /
 * _UNQUALIFIED / _NO_NEIGHBORS) now lands on the SAME source a genuine
 * solve moments earlier would have used, not a different one. Safe on every
 * board shipping today: the helper degrades to `z_ff_k_dc` whenever the
 * flag is off (the shipped default, `s_coupling_use_measured_diag_k_dc =
 * false`) or `coupling_diag_k_dc` is unset/non-finite/<=0 -- i.e. this
 * change is a no-op everywhere except a board that both flips the flag on
 * AND has `coupling_diag_k_dc` populated for zi, same rollout gate sec 3.2
 * already established for the matrix path. See
 * test_zone_coupling_solve.c's fallback-diagonal cases (n==1 and
 * out-of-range/unqualified paths, flag on vs off, measured vs unmeasured). */
float zone_coupling_solve_hold(bool z_qualifies, float z_ff_k_dc, uint8_t zi, bool use_measured_diag_k_dc,
                               const zone_coupling_neighbor_t *zones, uint8_t zone_count,
                               float setpoint_c, float ambient_c, bool *out_used_matrix, bool *out_infeasible,
                               coupling_solve_reason_t *out_reason, bool *out_membership_changed,
                               zone_coupling_hold_cache_t *cache_row, uint16_t *prev_membership_sig);

/* Same cache shape as zone_coupling_hold_cache_t, except `b` is one value PER
 * MEMBER (b[row], parallel to members[row]/G[row][*]) instead of a single
 * scalar -- the climb term's RHS is rate_c_per_s * tau_of_that_row's_zone,
 * and tau differs per zone (unlike the hold term's dT, which is the same
 * setpoint_c-ambient_c for every row -- see zone_coupling_solve_hold()'s
 * "LOAD-BEARING INVARIANT" comment). Kept as its own type, not a reuse of
 * zone_coupling_hold_cache_t with b widened, so a hold-cache/climb-cache mixup
 * is a compile error rather than a silent aliasing bug. */
typedef struct {
    bool    have;
    uint8_t n;
    uint8_t members[MAX31856_CHANNEL_COUNT];
    float   G[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    float   b[MAX31856_CHANNEL_COUNT];
    float   u[MAX31856_CHANNEL_COUNT];
    coupling_solve_reason_t reason;
    bool    infeasible;
} zone_coupling_climb_cache_t;

/* The coupled climb term for zone zi: how much EXTRA duty (beyond the hold
 * term) every zone in zi's qualifying system needs so that the commanded
 * ramp rate is achieved given cross-zone heat sharing, instead of each zone
 * independently chasing rate_c_per_s*tau_i/k_dc as though it heated alone
 * (the legacy formula, still returned here as the fallback for every
 * degraded case -- same contract as zone_coupling_solve_hold()).
 *
 * Derivation: a zone tracking a ramp at rate_c_per_s needs its own
 * temperature to be running `rate_c_per_s * tau_i` degrees ahead of the
 * quasi-steady value the current duty would otherwise settle to -- the same
 * dT-per-unit-duty steady-state relationship the hold term solves, just with
 * that lead as the target offset instead of (setpoint-ambient). So this
 * solves the SAME matrix A (the SAME G here, with the SAME membership) as
 * zone_coupling_solve_hold() for the same zi at the same tick -- only the
 * right-hand side changes -- via
 * zone_coupling_gauss_solve_partial_pivot_vec(), the vector-RHS sibling of
 * the exact routine zone_coupling_solve_hold() calls; there is no second
 * solver.
 *
 * z_ff_tau_s is zi's OWN tau, passed separately for the same reason
 * z_qualifies/z_ff_k_dc are in zone_coupling_solve_hold(): zi's row may not
 * be in `zones[]` at index zi (the caller's `z` need not BE zones[zi]).
 * Every OTHER member's tau comes from zones[j].ff_tau_s.
 *
 * out_membership_changed/prev_membership_sig exist for the same cache-miss/
 * membership-edge bookkeeping zone_coupling_solve_hold() does, tracked in
 * this function's OWN cache row and signature slot -- kept independent of the
 * hold term's even though the qualifying-neighbour criteria (and therefore,
 * in practice, the membership set) are identical, so a hold-cache bug can
 * never silently corrupt the climb answer or vice versa. Callers are not
 * obliged to surface this edge separately if they already reseed on the hold
 * term's identical edge; see the call site's own comment for what this
 * codebase does.
 *
 * use_measured_diag_k_dc: same flag, same guarded-fallback semantics as
 * zone_coupling_solve_hold()'s own doc comment -- kept independent per call
 * (not cached/shared) so a caller could in principle run the hold and climb
 * terms under different diagonal sources, though every caller in this tree
 * passes the same value to both. */
float zone_coupling_solve_climb(bool z_qualifies, float z_ff_k_dc, float z_ff_tau_s, uint8_t zi,
                                bool use_measured_diag_k_dc,
                                const zone_coupling_neighbor_t *zones, uint8_t zone_count,
                                float rate_c_per_s, bool *out_used_matrix, bool *out_infeasible,
                                coupling_solve_reason_t *out_reason, bool *out_membership_changed,
                                zone_coupling_climb_cache_t *cache_row, uint16_t *prev_membership_sig);

#ifdef __cplusplus
}
#endif

#endif // ZONE_COUPLING_SOLVE_H
