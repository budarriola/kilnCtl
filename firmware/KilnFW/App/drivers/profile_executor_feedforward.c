/* Cross-zone coupling wrappers, the identified-plant feedforward term, and
 * bumpless-transfer seeding -- split out of profile_executor.c (2026-09-01,
 * "files over 1500 lines should be broken up where it makes sense"). See
 * profile_executor_internal.h's own doc comment for the full three-way split
 * this is one piece of, and zone_coupling_solve.h for the actual Gaussian-
 * elimination/coupled-hold math these wrap -- that module was already
 * factored out for host-testability before this split. */

#include "profile_executor_internal.h"

#include <math.h>

#include "esp_log.h"

#include "zones_http.h"

/* zones_config_get_ease_off_window_mult() (zones_config_json.h/
 * zones_config_accessors.c) and its ZONE_EASE_OFF_WINDOW_MULT_DEFAULT
 * fallback constant -- forward-declared here rather than #include
 * "zones_config_json.h" deliberately: that header pulls in zones_cfg_t and
 * its _Static_assert()-guarded historical layouts (zone_cfg_v9_t..v14_t),
 * which test_profile_executor_prestart.c's host-test translation unit
 * compiles WITHOUT /std:c11 (unlike test_zones_http.c's own build, which
 * does) -- MSVC's default (pre-C11) mode does not recognize _Static_assert
 * as a keyword there, and every one of those asserts breaks the build. A
 * bare prototype and one small constant is a far smaller seam to duplicate
 * than moving a compiler-standard flag around a shared test script. Keep
 * this in sync with zones_config_json.h's own declaration/definition;
 * ZONE_EASE_OFF_WINDOW_MULT_DEFAULT's value (2.0f) must keep matching that
 * header's own #define -- see this field's ZONES_CFG_VERSION 15->16 doc
 * comment there for why. */
#define ZONE_EASE_OFF_WINDOW_MULT_DEFAULT 2.0f
bool zones_config_get_ease_off_window_mult(uint8_t zone_index, float *out_mult);

/* Terminal ease-off taper window, as a multiple of a zone's own identified
 * dead time -- see zone_taper_climb_rate()'s doc comment. Was a compile-time
 * #define (2.0f, per sim_calibration.md sec 5: the calibrated sim measured
 * 2.0x outperforming 1.0x on every zone's dwell-entry overshoot with linear
 * and cosine shapes statistically equivalent), now a runtime knob
 * (zones_cfg_t::ease_off_window_mult, ZONES_CFG_VERSION 15->16) so the 2.0x
 * sizing -- chosen against a simulator later found to undershoot dwell-entry
 * overshoot -- can be A/B tested on real hardware without a reflash between
 * arms. zone_taper_climb_rate() below reads the live value through
 * zones_config_get_ease_off_window_mult() every call rather than caching it,
 * so a config change taking effect mid-run (the whole point of an A/B swap
 * that must not require a reflash) is not a special case -- it is simply
 * what happens on the very next tick. */

/* ---- feedforward from the identified plant model (TODO.md 6A.2) ------------
 *
 *   u_ff = (T_sp - T_ambient)/K_dc + (dT_sp/dt)*tau/K_dc
 *
 * The first term is the duty the kiln needs just to HOLD the current setpoint
 * against its own losses; the second is the extra needed to CLIMB at the
 * commanded rate (energy going into the thermal mass rather than out through
 * the walls). With a plant whose dead time is tens of seconds, feedback alone
 * always trails a ramp by a roughly fixed offset -- the integrator can only
 * build that offset by first being wrong for long enough. Feedforward supplies
 * it from the model instead, and leaves the PID correcting only the model's
 * error, which is what puts the actual curve ON the desired curve.
 *
 * It is an INPUT to pid_update_terms(), never a bypass of it: the sum is
 * clamped there along with P+I+D, and every relay it can lead to still goes
 * through heater_output, the load cap, the guards and
 * relay_authority_zone_blocked() unchanged. Nothing here can command heat that
 * the rest of the tick would have refused. */

/* Reads zone zi's model out of zone config and decides whether it can be used
 * at all. Returns true if the verdict CHANGED (so callers can log an operator-
 * visible transition). The validation is deliberately total rather than
 * defensive-in-places: K_dc appears in a denominator in both terms, so a zero,
 * negative or non-finite model must turn feedforward OFF for the zone, not
 * produce an infinity that the clamp downstream would render as 100% duty --
 * a plausible-looking number arrived at by dividing by nothing. A negative
 * K_dc is likewise rejected rather than used: it asserts that adding duty
 * cools the kiln, which means the fit is wrong, not that the kiln is strange.
 * Must be called with s_exec.lock held. */
bool zone_load_model(uint8_t zi)
{
    zone_runtime_t *z = &s_exec.zones[zi];
    float k_dc = 0.0f, tau_s = 0.0f, dead_time_s = 0.0f;

    /* zones_http.h's contract: false means "cannot answer", and 0 in any of
     * the three outputs means "no model has been identified for this zone" --
     * the expected state of a zone that has never been autotuned, not an
     * error. Either way there is nothing to compute with. */
    bool have = zones_config_get_model(zi, &k_dc, &tau_s, &dead_time_s) &&
                isfinite(k_dc) && isfinite(tau_s) && isfinite(dead_time_s) &&
                k_dc > 0.0f && tau_s > 0.0f && dead_time_s > 0.0f;

    bool was_enabled = z->ff_enabled;
    float was_k = z->ff_k_dc, was_tau = z->ff_tau_s, was_dead_time = z->ff_dead_time_s;

    z->ff_enabled = have;
    z->ff_k_dc = have ? k_dc : 0.0f;
    z->ff_tau_s = have ? tau_s : 0.0f;
    z->ff_dead_time_s = have ? dead_time_s : 0.0f;

    return (z->ff_enabled != was_enabled) || (z->ff_k_dc != was_k) || (z->ff_tau_s != was_tau) ||
           (z->ff_dead_time_s != was_dead_time);
}

/* Cross-zone coupling/feedforward math (Gaussian elimination, the coupled
 * steady-state hold solve, the neighbour-side low-pass filter tick, and the
 * neighbour-qualification predicate) now lives in zone_coupling_solve.c/.h --
 * split out for host-testability the same way dashboard_json.c and
 * zones_config_json.c were (see zone_coupling_solve.h's own doc comment for
 * why). The four thin wrappers below exist only because that module's
 * functions take plain scalars/a small zone_coupling_neighbor_t array
 * instead of this file's private zone_runtime_t* -- every call site below
 * this point keeps calling coupling_filter_tick()/
 * zone_qualifies_as_coupling_neighbor()/count_qualifying_coupling_neighbors()/
 * solve_hold_for_zone() by the same names and signatures as before the
 * split; only where these four are DEFINED changed. The cache and
 * membership-signature arrays (s_coupling_hold_cache[]/
 * s_coupling_prev_membership_sig[]) stay right here, unchanged, and are
 * threaded into zone_coupling_solve_hold() by reference to this zi's own
 * slot -- see that function's doc comment. */

void coupling_filter_tick(zone_runtime_t *zn, bool actual_valid_now, float dt_s)
{
    zone_coupling_filter_tick(&zn->coupling_filtered_c, &zn->coupling_filter_init, zn->actual_c,
                              actual_valid_now, zn->pid_cfg.d_filter_tau_s, dt_s);
}

static bool zone_qualifies_as_coupling_neighbor(const zone_runtime_t *zn)
{
    return zone_coupling_qualifies_as_neighbor(zn->active, zn->actual_valid, zn->actual_c, zn->ff_enabled,
                                               zn->ff_k_dc, zn->control_mode, zn->faulted, zn->heat_blocked);
}

/* Builds the per-zone {qualifies, ff_k_dc} array zone_coupling_solve.c's
 * neighbour-facing functions take, straight from s_exec.zones[] -- every
 * OTHER zone's own fields are always read from s_exec.zones[], both before
 * and after this split (see solve_hold_for_zone()'s wrapper below for the
 * one exception: zi's OWN entry, which callers pass separately since `z` is
 * not guaranteed to BE s_exec.zones[zi]). Must be called with s_exec.lock
 * held (reads s_exec.zones[]). */
static void build_coupling_neighbor_array(zone_coupling_neighbor_t out[MAX31856_CHANNEL_COUNT])
{
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        out[j].qualifies = zone_qualifies_as_coupling_neighbor(&s_exec.zones[j]);
        out[j].ff_k_dc = s_exec.zones[j].ff_k_dc;
        out[j].ff_tau_s = s_exec.zones[j].ff_tau_s;
    }
}

uint8_t count_qualifying_coupling_neighbors(uint8_t zi)
{
    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    build_coupling_neighbor_array(zones);
    return zone_coupling_count_qualifying_neighbors(zi, zones, MAX31856_CHANNEL_COUNT);
}

static zone_coupling_hold_cache_t s_coupling_hold_cache[MAX31856_CHANNEL_COUNT];
static uint16_t s_coupling_prev_membership_sig[MAX31856_CHANNEL_COUNT];

/* PID_EXPANSION_PLAN.md sec 3.2 ("STORAGE LANDED 2026-09-02f" / "the solver
 * switch itself"): whether the coupled hold/climb solve prefers the coupling
 * matrix's own measured diagonal cell (coupling_diag_k_dc) over the step-
 * identified ff_k_dc, when the former is available. Deliberately false --
 * the plan section explicitly scopes "flip this on" as its own, separately
 * reviewed decision: coupling_diag_k_dc has no autotune writer on this board
 * today (hand-set/PC-preset only), and whether the UNCOUPLED 1x1 fallback
 * (diagonal_hold/diagonal_climb in zone_coupling_solve.c, which never routes
 * through this flag at all) should switch too is a separate, unresolved
 * question this flag does not answer. Flipping this one constant is now the
 * entire remaining step -- see zone_coupling_solve.h's own doc comment on
 * `use_measured_diag_k_dc` for the guarded-fallback contract this enables. */
static const bool s_coupling_use_measured_diag_k_dc = false;

static float solve_hold_for_zone(const zone_runtime_t *z, uint8_t zi, float setpoint_c, float ambient_c,
                                  bool *out_used_matrix, bool *out_infeasible,
                                  coupling_solve_reason_t *out_reason, bool *out_membership_changed)
{
    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    build_coupling_neighbor_array(zones);
    bool z_qualifies = zone_qualifies_as_coupling_neighbor(z);
    zone_coupling_hold_cache_t *cache_row = (zi < MAX31856_CHANNEL_COUNT) ? &s_coupling_hold_cache[zi] : &s_coupling_hold_cache[0];
    uint16_t *prev_sig = (zi < MAX31856_CHANNEL_COUNT) ? &s_coupling_prev_membership_sig[zi] : &s_coupling_prev_membership_sig[0];
    return zone_coupling_solve_hold(z_qualifies, z->ff_k_dc, zi, s_coupling_use_measured_diag_k_dc, zones,
                                    MAX31856_CHANNEL_COUNT, setpoint_c,
                                    ambient_c, out_used_matrix, out_infeasible, out_reason,
                                    out_membership_changed, cache_row, prev_sig);
}

/* Climb-term counterpart of solve_hold_for_zone() above -- own cache/
 * membership-signature storage (zone_coupling_climb_cache_t's b[] differs in
 * shape from the hold cache's scalar b, so they cannot share a slot), same
 * wrapping pattern otherwise. See zone_feedforward()'s call site for why the
 * climb term's own out_membership_changed is not surfaced as a second
 * zone_runtime_t field -- the qualifying-neighbour criteria are identical to
 * the hold term's, so it is always the same tick's edge. */
static zone_coupling_climb_cache_t s_coupling_climb_cache[MAX31856_CHANNEL_COUNT];
static uint16_t s_coupling_climb_prev_membership_sig[MAX31856_CHANNEL_COUNT];

static float solve_climb_for_zone(const zone_runtime_t *z, uint8_t zi, float rate_c_per_s,
                                   bool *out_used_matrix, bool *out_infeasible,
                                   coupling_solve_reason_t *out_reason, bool *out_membership_changed)
{
    zone_coupling_neighbor_t zones[MAX31856_CHANNEL_COUNT];
    build_coupling_neighbor_array(zones);
    bool z_qualifies = zone_qualifies_as_coupling_neighbor(z);
    zone_coupling_climb_cache_t *cache_row = (zi < MAX31856_CHANNEL_COUNT) ? &s_coupling_climb_cache[zi] : &s_coupling_climb_cache[0];
    uint16_t *prev_sig = (zi < MAX31856_CHANNEL_COUNT) ? &s_coupling_climb_prev_membership_sig[zi] : &s_coupling_climb_prev_membership_sig[0];
    return zone_coupling_solve_climb(z_qualifies, z->ff_k_dc, z->ff_tau_s, zi, s_coupling_use_measured_diag_k_dc,
                                     zones, MAX31856_CHANNEL_COUNT,
                                     rate_c_per_s, out_used_matrix, out_infeasible, out_reason,
                                     out_membership_changed, cache_row, prev_sig);
}


/* Terminal ease-off (PID_EXPANSION_PLAN.md sec 3.1). Tapers the FEEDFORWARD's
 * rate input toward zero as the ramp approaches its segment's own target --
 * `s_exec.target_c`/the setpoint schedule itself is NEVER touched here, only
 * the number this zone's zone_feedforward() call is handed as rate_c_per_s.
 * That is deliberate, not incidental: because the schedule that advances
 * target_c is computed elsewhere (profile_executor.c's segment-stepping
 * block) and this function cannot see or change it, a taper bug here can, at
 * worst, under-supply the climb feedforward near the end of a ramp -- the
 * PID's own P/I terms still chase the untapered target_c and correct the
 * shortfall like any other tracking error. It cannot turn into a stall: the
 * segment still reaches c1 on the untouched wall-clock schedule regardless
 * of what this returns.
 *
 * Validated in sim_calibration.md sec 5 (calibrated_sim.py, correct-rate
 * core): window_mult=2.0 * the zone's own identified dead time, taper keyed
 * to wall-clock time remaining in the ramp (distance / rate, matching the
 * segment's own constant-rate schedule) roughly halves seg0-dwell RMS
 * overshoot on z0/z1 for a 0.02-0.08C ramp-window cost, comparable to the
 * calibrated sim's own ~1C residual-vs-hardware noise floor. The sim found
 * linear and cosine shapes statistically equivalent at this window; linear
 * is kept here (one fewer transcendental per tick, easier to host-test
 * exactly).
 *
 * Per-zone, off the zone's OWN ff_dead_time_s -- never a hand constant tuned
 * to this kiln -- so a short-dead-time zone (z2 here) eases later and over a
 * shorter absolute window than a long-dead-time zone (z0). A zone with no
 * identified dead time (never autotuned) gets rate_c_per_s back unchanged:
 * there is no real per-zone window to size the taper from, and fabricating
 * one would be exactly the kind of hand constant this is required not to be.
 *
 * rate_c_per_s == 0 (dwelling, a step segment, or a ramp-lock stall -- see
 * this function's callers for the dwelling/nonzero-rate gate) returns 0
 * unconditionally: multiplying zero by any taper factor is still zero, but
 * skipping the division avoids a 0/0 on a segment with zero distance left. */
float zone_taper_climb_rate(const zone_runtime_t *z, uint8_t zi, float target_c, float rate_c_per_s,
                            float segment_target_c)
{
    if (rate_c_per_s == 0.0f) return 0.0f;
    if (!(z->ff_dead_time_s > 0.0f)) return rate_c_per_s;

    float dist_c = fabsf(segment_target_c - target_c);
    float dist_s = dist_c / fabsf(rate_c_per_s);
    /* Runtime, PER-ZONE multiplier (ZONES_CFG_VERSION 16->17) -- see this
     * file's top-of-file comment on why this reads through the accessor
     * every call rather than caching it, and zones_config_get_ease_off_
     * window_mult()'s own comment for why it always returns a legal,
     * safe-to-multiply-by value even against a zeroed/unconfigured
     * zones_cfg_t. Zone `zi`'s own value only -- this is what makes a
     * z0-only override possible without touching z1/z2's. */
    float mult = ZONE_EASE_OFF_WINDOW_MULT_DEFAULT;
    zones_config_get_ease_off_window_mult(zi, &mult);
    float window_s = mult * z->ff_dead_time_s;

    if (dist_s >= window_s) return rate_c_per_s;
    if (dist_s <= 0.0f) return 0.0f;

    float f = dist_s / window_s; /* linear taper, x in (0,1) */
    return rate_c_per_s * f;
}

/* The feedforward duty for one zone, already clamped to [0,1]. Returns exactly
 * 0.0f -- i.e. the behaviour of every firing before this existed -- for a zone
 * with no usable model.
 *
 * The SUM is clamped, not each term: on a cooling ramp the second term is
 * legitimately negative and is supposed to reduce the hold duty below what a
 * steady hold would need. Clamping the terms separately would throw that away
 * and hold the kiln up through a controlled cool.
 *
 * out_hold (optional, NULL-able): the steady-state HOLD-only portion of the
 * returned total -- everything EXCEPT the climb/ramp term, i.e. `hold` plus
 * the Phase-3b cross-zone coupling correction below (that correction is a
 * steady-state term, not a rate term -- see its own comment). Deliberately
 * NOT independently clamped to [0,1]: only the joint hold+climb sum is
 * clamped (matching this function's own return value), so a caller reading
 * *out_hold directly can see a value >1.0 or <0.0 when the joint sum needed
 * clamping -- e.g. a cooling ramp where hold alone exceeds 1.0 but
 * hold+climb does not. This is the 2026-08-31 "hold-only integral floor" fix
 * (pid.c's floor is -ff_hold, not -ff_u, so it never cancels the climb
 * term) -- see pid.h's top-of-file doc comment for the full rationale. */
float zone_feedforward(const zone_runtime_t *z, uint8_t zi, float setpoint_c, float rate_c_per_s,
                              float *out_hold)
{
    if (!z->ff_enabled) {
        if (out_hold) *out_hold = 0.0f;
        return 0.0f;
    }
    bool hold_used_matrix = false, hold_infeasible = false, hold_membership_changed = false;
    coupling_solve_reason_t hold_reason = COUPLING_SOLVE_OK;
    float hold = solve_hold_for_zone(z, zi, setpoint_c, s_exec.ambient_c, &hold_used_matrix, &hold_infeasible,
                                     &hold_reason, &hold_membership_changed);

    /* Climb term: was `(rate_c_per_s * z->ff_tau_s) / z->ff_k_dc`, the same
     * per-zone-as-though-it-heated-alone formula the hold term used before
     * TODO.md/PID_EXPANSION_PLAN.md's coupled-hold fix -- see
     * zone_coupling_solve_climb()'s doc comment (zone_coupling_solve.h) for
     * the coupled derivation. Own diagnostics/degrade path, mirroring the
     * hold term's, deliberately not sharing hold's out_membership_changed:
     * the two solves share the same qualifying-neighbour criteria, so a
     * membership edge on this tick is always caught by hold_membership_changed
     * already and does not need a second reseed trigger here. */
    bool climb_used_matrix = false, climb_infeasible = false, climb_membership_changed = false;
    coupling_solve_reason_t climb_reason = COUPLING_SOLVE_OK;
    float climb = solve_climb_for_zone(z, zi, rate_c_per_s, &climb_used_matrix, &climb_infeasible,
                                       &climb_reason, &climb_membership_changed);
    /* hold_total accumulates `hold` plus the Phase-3b cross-zone coupling
     * correction below -- everything that belongs on the floor's hold side,
     * never climb. `u_ff` (climb included) is still what gets clamped and
     * returned; hold_total is what *out_hold reports, unclamped. */
    float hold_total = hold;

    /* Diagnostics written to s_exec.zones[zi], not through `z` -- z is const
     * here, and (see solve_hold_for_zone()'s doc comment) is not guaranteed
     * to BE s_exec.zones[zi] in the first place, only its production-code
     * value. s_exec.zones[zi] is always the real per-zone runtime record
     * that profile_executor_get_status() reports off-board (Opus review,
     * blocker 3: wired through get_status()/dashboard_http.c's JSON, the
     * same path heat_blocked already uses -- this comment's claim is now
     * actually true, not aspirational). Logged once per TRANSITION only
     * (apply_relay()'s own heat_blocked logging uses the same discipline) --
     * this runs every tick. ff_membership_changed is read (and cleared by
     * being re-derived) every tick by pid_family_zone_tick() immediately
     * after this call, to decide whether to re-seed -- see that call site. */
    if (zi < MAX31856_CHANNEL_COUNT) {
        s_exec.zones[zi].ff_hold_used_matrix = hold_used_matrix;
        s_exec.zones[zi].ff_hold_infeasible = hold_infeasible;
        s_exec.zones[zi].ff_hold_reason = (uint8_t)hold_reason;
        s_exec.zones[zi].ff_climb_used_matrix = climb_used_matrix;
        s_exec.zones[zi].ff_climb_infeasible = climb_infeasible;
        s_exec.zones[zi].ff_climb_reason = (uint8_t)climb_reason;
        s_exec.zones[zi].ff_membership_changed = hold_membership_changed || climb_membership_changed;
        static bool s_fallback_active[MAX31856_CHANNEL_COUNT];
        static bool s_infeasible_active[MAX31856_CHANNEL_COUNT];
        bool fell_back = !hold_used_matrix;
        if (fell_back != s_fallback_active[zi]) {
            s_fallback_active[zi] = fell_back;
            if (fell_back) {
                ESP_LOGW(PE_TAG, "zone %u coupled feedforward hold fell back to the legacy per-zone "
                              "diagonal (unqualified zone, no qualifying coupled neighbours, or a "
                              "singular/ill-conditioned coupling matrix)",
                         zi);
            } else {
                ESP_LOGI(PE_TAG, "zone %u coupled feedforward hold resumed using the solved coupling matrix", zi);
            }
        }
        if (hold_infeasible != s_infeasible_active[zi]) {
            s_infeasible_active[zi] = hold_infeasible;
            if (hold_infeasible) {
                ESP_LOGW(PE_TAG, "zone %u coupled feedforward solve required clamping a zone's duty to "
                              "[0,1] -- the commanded setpoint combination is not achievable as specified",
                         zi);
            } else {
                ESP_LOGI(PE_TAG, "zone %u coupled feedforward solve back within [0,1] without clamping", zi);
            }
        }

        /* Opus review round 3, item 3: the membership-transition reseed
         * (pid_family_zone_tick(), on hold_membership_changed) makes a
         * chattering membership a SAFE failure mode (a sustained FF+P
         * offset while the integrator effectively stops correcting,
         * instead of a repeated 6-10 point duty step) but it is a silent
         * one -- heat_blocked alone is refreshed every tick by
         * apply_relay(), so a flapping interlock can drive
         * seed_bumpless_with_ff() every single tick with nothing to notice
         * it happening. Counted here (once per genuine membership change,
         * the same edge the reseed itself reacts to) and surfaced in status
         * (get_status()/dashboard_http.c's JSON, same path as the flags
         * just above) so a chattering membership is visible off-board
         * rather than only inferable from a sustained control-error offset
         * nobody thought to attribute to this. Logged at a RATE-LIMITED
         * cadence, not once per occurrence -- the first change (every
         * commissioned kiln sees a few of these across a run, e.g. autotune
         * finishing on a neighbour) and then every 10th after that, so a
         * genuinely chattering interlock produces a growing but bounded
         * trickle of log lines instead of flooding the UART bridge the way
         * this file's own LOG_PRESTART_ONCE doc comment describes that
         * queue dropping lines under. */
        if (hold_membership_changed) {
            s_exec.zones[zi].ff_membership_change_count++;
            uint32_t count = s_exec.zones[zi].ff_membership_change_count;
            if (count == 1 || count % 10 == 0) {
                ESP_LOGW(PE_TAG, "zone %u coupled feedforward membership changed (count=%lu this run) -- "
                              "PID integral re-seeded to hold commanded duty steady; a fast-growing "
                              "count means a flapping interlock/coupled-neighbour state is leaving "
                              "integral action effectively off for this zone",
                         zi, (unsigned long)count);
            }
        }
    }

    /* PID_EXPANSION_PLAN.md section 2c / Phase 3b: additive cross-zone
     * coupling contribution, added here so it stays inside the isfinite/
     * clamp below rather than escaping it.
     *
     * coupling_coeff[j] (zones_http.h) is zone zi's measured steady-state
     * response in raw degC PER UNIT DUTY AT ZONE j'S HEATER -- the exact
     * same convention as ff_k_dc/model_k_dc, deliberately not a
     * dimensionless ratio. That means a plain `-c_ij*(T_j-sp_j)` is not a
     * duty: it mixes (degC_i/duty_j) with degC_j into degC_i*degC_j/duty_j,
     * which silently rescales the whole term by whatever K_dc happens to be.
     * The dimensionally correct form is the standard measured-disturbance
     * feedforward [W6]/[10]:
     *
     *     u_ff_d = -(Gd / Gu) * d
     *
     * with the manipulated-variable gain Gu = this zone's own
     * d(T_i)/d(duty_i) = z->ff_k_dc, and the disturbance gain Gd =
     * d(T_i)/d(T_j) -- a dimensionless degC_i-per-degC_j ratio, NOT
     * coupling_coeff[j] itself. Gd is recovered by dividing coupling_coeff[j]
     * (degC_i per duty_j) by neighbour j's OWN steady-state gain k_dc_j
     * (degC_j per duty_j); the duty_j unit cancels, leaving degC_i/degC_j:
     *
     *     Gd_ij = coupling_coeff[j] / k_dc_j
     *     term  = -(Gd_ij / z->ff_k_dc) * (T_j - setpoint_j)
     *           = -(coupling_coeff[j] / (k_dc_j * z->ff_k_dc)) * (T_j - setpoint_j)
     *
     * A neighbour running hot (T_j > setpoint_j) subtracts duty from this
     * zone; a neighbour running cold adds it; a neighbour on target changes
     * nothing. The zone-wide setpoint is shared (s_exec.target_c, see its
     * own doc comment), so neighbour j's setpoint is the very same
     * setpoint_c already passed in for zone zi -- no separate lookup needed.
     *
     * Skipped, contributing exactly 0 and never NaN: the diagonal (j==zi),
     * any coupling_coeff[j] that is 0/non-finite (0 is coupling_coeff's own
     * "unmeasured" default -- with every row 0 this loop changes nothing,
     * which is finding 1/the required zero-coefficient parity), any
     * neighbour with no identified k_dc_j of its own (Gd_ij is not
     * computable without it, so "no model for that neighbour" degrades to
     * the same "contribute 0" default as "no coupling measured"), and any
     * neighbour zone_qualifies_as_coupling_neighbor() rejects -- inactive,
     * no valid reading, not currently under PID-family closed-loop control
     * on this shared setpoint, faulted, or blocked by
     * relay_authority_zone_blocked(). That last group is a post-review
     * fix (was just active/reading-valid before): a neighbour sitting at
     * ZONE_CONTROL_MODE_OFF or authority-blocked can be arbitrarily far
     * from setpoint for reasons that have nothing to do with its own
     * heater, so attributing that gap to "this zone's duty is doing
     * something" was simply wrong -- see zone_qualifies_as_coupling_neighbor()'s
     * doc comment for the concrete failure this was producing. The
     * deviation itself is also low-pass filtered and bounded before it's
     * scaled -- see the two comments inside the loop below. */
    /* ROADMAP.md M15 B4: the coupling correction folded into hold_total
     * below, broken out into its own accumulator purely for reporting --
     * mirrors hold_total's own accumulation term-for-term, never read by
     * anything that affects control. */
    float coupling_correction = 0.0f;
    float coupling_row[MAX31856_CHANNEL_COUNT];
    if (zones_config_get_coupling(zi, coupling_row)) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (j == zi) continue;
            float c_ij = coupling_row[j];
            if (!isfinite(c_ij) || c_ij == 0.0f) continue;
            const zone_runtime_t *zn = &s_exec.zones[j];
            if (!zone_qualifies_as_coupling_neighbor(zn)) continue;
            float gd_ij = c_ij / zn->ff_k_dc;

            /* Neighbour deviation: the LOW-PASS FILTERED reading (see the
             * coupling_filtered_c update site), not the raw one -- finding
             * 3, keeps thermocouple noise out of this zone's duty. Bounded
             * to +/- zn->pid_cfg.pid_range_c (finding 2) before it is
             * scaled: that is the exact threshold pid.c itself already uses
             * to decide a zone's error is too large for the linear PID/FF
             * model to mean anything (pid_range_c gate in
             * pid_update_terms(), full on/off outside it) -- reusing it
             * here means a deviation this term is allowed to react to
             * linearly is never larger than one pid.c itself would still
             * be doing linear control on. Without this a neighbour idle at
             * OFF used to pass everything else and reach a 600C deviation
             * (the reviewer's scenario) uncapped; with a genuinely
             * qualifying neighbour (PID family, not faulted, not blocked)
             * a multi-hundred-degree gap should not occur in the first
             * place, so this bound is a backstop against a bad reading or
             * a slow-to-settle transient, not the primary defense -- that
             * is finding 1's control_mode/faulted/heat_blocked gate above.
             *
             * coupling_filter_init is only set once the main control loop
             * has actually run at least one tick for the neighbour (the
             * filter update site above). A caller that reaches
             * zone_feedforward() before that (the host tests below, which
             * poke actual_c directly and call this function with no tick
             * loop around it) falls back to the raw actual_c -- the same
             * "snap to the first sample" the filter itself does on its own
             * first update, so this is not a second, divergent behaviour,
             * just the filter's own cold-start case reached a different
             * way. */
            float neighbour_temp = zn->coupling_filter_init ? zn->coupling_filtered_c : zn->actual_c;
            float dev = neighbour_temp - setpoint_c;
            float bound = zn->pid_cfg.pid_range_c;
            if (bound > 0.0f) {
                if (dev > bound) dev = bound;
                else if (dev < -bound) dev = -bound;
            }
            float term = (gd_ij / z->ff_k_dc) * dev;
            hold_total -= term;
            coupling_correction -= term;
        }
    }

    /* ROADMAP.md M15 B4: Stage A of the duty breakdown -- read-only, see
     * zone_duty_breakdown_t's doc comment. hold_total/climb here are the
     * exact unclamped values *out_hold/u_ff below are built from. */
    if (zi < MAX31856_CHANNEL_COUNT) {
        s_exec.zones[zi].duty_breakdown.ff_hold = hold_total;
        s_exec.zones[zi].duty_breakdown.ff_climb = climb;
        s_exec.zones[zi].duty_breakdown.coupling_correction = coupling_correction;
    }

    float u_ff = hold_total + climb;
    if (!isfinite(u_ff)) {
        if (out_hold) *out_hold = 0.0f;
        return 0.0f; /* belt-and-braces: a non-finite setpoint can only come
                      * from a corrupted profile, but it must not become duty */
    }
    /* out_hold is reported here, from the unclamped hold_total, BEFORE the
     * joint clamp below touches u_ff -- see this function's own doc comment
     * for why it must stay unclamped. */
    if (out_hold) *out_hold = hold_total;
    if (u_ff < 0.0f) u_ff = 0.0f;
    if (u_ff > 1.0f) u_ff = 1.0f;
    return u_ff;
}

/* Seeds the PID integral so the very next tick (which computes
 * P + I + D + ff) reproduces u_desired -- pid_seed_bumpless() itself now
 * subtracts ff_u before solving for the integral (TODO.md 6A.2's "move the
 * feedforward subtraction into pid_seed_bumpless()"), so this wrapper only
 * has to compute the feedforward term the next tick will use and hand it
 * over alongside u_desired.
 *
 * When u_ff alone already exceeds u_desired the shortfall cannot be expressed
 * -- the integral floor is 0, since a negative one violates the anti-windup
 * clamp on the next tick -- so the zone comes back at its feedforward duty.
 * That is the honest outcome rather than a defect: the model's estimate of
 * what the current setpoint costs to hold is exactly what the operator asked
 * to resume onto. Must be called with s_exec.lock held.
 *
 * zi must be z's own index in s_exec.zones[] -- zone_feedforward() needs it
 * both to look up z's own coupling row (PID_EXPANSION_PLAN.md section 2c)
 * and to skip that row's own diagonal. This is the same zone_feedforward()
 * called from the per-tick control loop (pid_family_zone_tick()), on the
 * same s_exec.target_c/target_rate_c_per_s -- deliberately identical inputs,
 * so a zone reseeded here is bumpless against exactly the feedforward the
 * very next tick will compute, coupling term included. If the two callers
 * ever diverge on what they pass, bump transfer breaks.
 *
 * Residual bump-transfer gap the reviewer flagged: "identical inputs" only
 * covers target_c/target_rate_c_per_s and zi -- it does NOT mean a
 * neighbour's contribution is frozen between this seed and the next real
 * tick. coupling_filtered_c (this zone's neighbour-side low-pass, see its
 * update site) is bumpless against itself -- both calls read whatever the
 * filter's current value is -- but that value keeps moving between ticks
 * exactly like any filtered signal does. If a neighbour's thermocouple
 * reading is recovering (e.g. it just came back in range) between this seed
 * and the next tick, the filtered deviation used here and the one used a
 * tick later can differ, and this zone's duty steps by
 * (gd_ij/z->ff_k_dc) * delta_dev -- the reviewer's example (gd/k =
 * 0.0159/degC, a 30C recovery = a 48% step) is against the raw,
 * pre-filter deviation. The d_filter_tau_s low-pass added here
 * (finding 3) softens this materially by construction: a low-pass turns a
 * step into an exponential approach over ~tau, so the SAME 30C recovery
 * spreads across many ticks instead of landing on whichever single tick
 * happened to seed. It does not eliminate the residual: a real
 * discontinuity in the neighbour's temperature (sensor recovering, not
 * just filtered noise) still passes through, delayed and attenuated rather
 * than blocked. No further machinery is added for this -- the same
 * anti-windup clamp that already bounds every other seed's error is the
 * backstop, and the filter is doing exactly what pid.c's own D filter does
 * for the analogous problem on the local sensor. */
void seed_bumpless_with_ff(zone_runtime_t *z, uint8_t zi, float u_desired)
{
    /* Same taper this zone's very next pid_family_zone_tick() call will
     * apply (gate on s_exec.dwelling, not merely target_rate_c_per_s != 0 --
     * a ramp-lock stall already zeros target_rate_c_per_s WITHOUT setting
     * dwelling, and zone_taper_climb_rate() returns 0 for a 0 rate either
     * way, but gating explicitly here keeps this call and pid_family_zone_
     * tick()'s identical by construction rather than by coincidence of the
     * zero-rate case) -- this function's own doc comment requires identical
     * inputs to the very next real tick for the seed to actually be
     * bumpless; an untapered seed racing against a tapered next tick would
     * reintroduce exactly the duty step this function exists to avoid. */
    float rate_c_per_s = s_exec.target_rate_c_per_s;
    if (!s_exec.dwelling && rate_c_per_s != 0.0f) {
        const profile_segment_t *seg = &s_exec.profile.segments[s_exec.segment_index];
        rate_c_per_s = zone_taper_climb_rate(z, zi, s_exec.target_c, rate_c_per_s, seg->target_c);
    }
    float ff_hold = 0.0f;
    float u_ff = zone_feedforward(z, zi, s_exec.target_c, rate_c_per_s, &ff_hold);
    pid_seed_bumpless(&z->pid_state, &z->pid_cfg, s_exec.target_c, z->actual_c, u_desired, u_ff, ff_hold);
}
