#include "adaptive_tune.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pid_autotune.h"
#include "zone_coupling_solve.h" // zone_coupling_gauss_solve_partial_pivot_vec() -- the ONLY thing of this
                                  // module's this file calls; never edited here (see PID_EXPANSION_PLAN.md
                                  // 3.3 and this file's coupled-fit comments for why reusing its conditioning
                                  // check is load-bearing, not cosmetic)
#include "zones_http.h" // zones_config_get_model/set_model/get_pid/set_pid/get_coupling/set_coupling_cell

// uart_bridge_ext.c's internal-SRAM-stack flash-write executor. Declared by
// hand rather than #include "uart_bridge.h" -- same reasoning safety_cfg_
// store.c's identical declaration gives: that header pulls in a pile of
// hardware bridge dependencies this file does not need, and which are not
// part of the host-test stub surface. A flash WRITE from a PSRAM-stacked
// task asserts on this board every time (see PID_EXPANSION_PLAN.md and
// project_psram_stack_nvs_panic); every NVS WRITE this file makes is
// dispatched through this worker for exactly that reason. Reads are not
// routed through it -- reading is not the hazard, only writing is (same
// distinction safety_cfg_store.c's own comments draw), and this module's
// only read (adaptive_tune_init()'s flag load) runs once at boot from
// app_main's task, which is not PSRAM-stacked.
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

static const char *TAG = "adaptive_tune";

// ---------------------------------------------------------------------
// Guards -- every numeric bound this module can refuse an update on.
// ---------------------------------------------------------------------

// Settling: reuses autotune_engine.c's check_thermal_readiness_locked()
// approach (stability-primary slope test) rather than a second settled-ness
// test, per PID_EXPANSION_PLAN.md Phase 7d-2 -- same numbers, redefined
// here by hand since autotune_engine.c is off-limits to edit or include
// (another agent holds it live) and neither constant is exported.
#define ADAPTIVE_TUNE_SETTLE_MIN_S 180.0f          // == AUTOTUNE_ENGINE_SETTLE_S
#define ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S 0.003f // == autotune_engine.c's SETTLE_ABS_SLOPE_FLOOR_C_PER_S

// A dwell duty below this is too close to the noise floor for a duty-vs-
// rise pair to mean anything -- a MAX31856 channel's practical noise floor
// is a few hundredths of a degree, and at 3% duty even a well-identified
// zone's rise is only a few degrees, so the signal-to-noise on the fit
// starts to collapse below this.
#define ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION 0.03f

// One ring per zone. Cost: 8 bytes/observation (two floats) * 12 * 5 zones
// = 480 bytes -- small enough that PSRAM buys nothing; kept as a plain
// static array rather than heap_caps_malloc'd PSRAM, unlike autotune's much
// larger sample trace.
#define ADAPTIVE_TUNE_RING_CAPACITY 12

// Refuse to fit with fewer than this many observations -- the plan's own
// worked example (a single dwell yields 3 equations against 9 unknowns for
// the full coupled matrix) scales down to "one point cannot separate a
// slope from noise" even for this diagonal-only 1-parameter fit. 4 is a
// deliberately conservative floor above the theoretical minimum of 2.
#define ADAPTIVE_TUNE_MIN_OBSERVATIONS 4u

// The stacked duty values must span at least this much (max-min) or the
// fit is effectively estimating a slope from one operating point repeated
// several times -- the 1-D analogue of the plan's rank/conditioning check
// on the full coupled matrix. Below this, refuse rather than report a
// confident number the data cannot support.
#define ADAPTIVE_TUNE_MIN_DUTY_SPREAD 0.05f

// A raw fit more than this multiple away from the existing model (in
// either direction) is treated as an implausible fit rather than a real
// change in the element -- a genuinely aged element drifts run over run,
// it does not 5x between one firing and the next. Guards against a
// contaminated observation (a stuck relay, a misread thermocouple) that
// slipped past the settle/spread checks.
#define ADAPTIVE_TUNE_MAX_JUMP_RATIO 5.0f

// Blend fraction per Phase 7d-2 ("start near 15%") and the per-run cap on
// how far the blended value may move from the prior one -- independent
// numbers so a single very-off fit cannot swing the model by more than
// MAX_FRACTIONAL_MOVE even if ALPHA alone would have allowed it (they
// currently coincide at the same value, but are separate knobs on purpose).
#define ADAPTIVE_TUNE_BLEND_ALPHA 0.15f
#define ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE 0.20f

// F1 fix: try_refine_zone_locked() used to report success (and write
// set_model()/set_pid()) on ANY nonzero blend, however small -- an
// asymptotically-converging sequence of fits (each blend a little closer to
// the true gain, never exactly equal) therefore reported "applied" on every
// single run forever. adaptive_tune_run_end()'s D5 policy is "the Ki
// diagnosis gets a turn on any run where the model refine did not fire" --
// but with no material-change floor, that run never arrived: 8 consecutive
// well-formed runs on one zone measured has_applied=1 every run (k_dc 10.0 ->
// 13.65, asymptotically approaching 15, never equal), so try_refine_ki_
// locked() never ran at all. A blended move smaller than this fraction of
// the prior K_dc is declared "no material change" and refused (like any
// other guard failure) rather than written -- this is what makes "the model
// refine did not fire" a REACHABLE case again, without stacking a Ki
// correction measured under a Ki value this same run is about to replace
// (see try_refine_ki_locked()'s own reasoning for why the two layers must
// not both act in one run). 0.5% is well below the smallest per-run move
// this file's own tests exercise deliberately (the bounded-move test caps at
// 20%).
//
// H4(a) correction: this guard does NOT fire "only once the blend has
// genuinely flattened out" onto the true gain -- it fires once the blend has
// flattened onto a PERMANENT, biased steady state short of the true gain.
// Each run moves k_dc by ALPHA*(k_true-k_dc); the guard refuses whenever
// that move is smaller than FRAC*k_dc. Setting the two equal and solving for
// the ratio k_dc/k_true gives the fixed point the sequence converges to and
// then freezes at:
//   k_dc / k_true = ALPHA / (ALPHA + FRAC) = 0.15 / 0.155 ~= 0.9677
// i.e. a permanent ~3.23% UNDERESTIMATE of the true gain, independent of
// what the true gain actually is -- measured: K_dc parks at 14.578 against a
// true gain of 15.000 (test_ki_diagnosis_eventually_runs_after_repeated_
// converging_refinements()) and never moves again. This is an accepted
// trade-off (a small, bounded, permanent bias in exchange for making "the
// model refine did not fire" reachable again), not a defect -- but it must
// not be described as full convergence.
#define ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC 0.005f

// A run whose zone lost more than this fraction of its samples to sensor
// dropout must not become training data -- same reasoning as Phase 7a's
// excluded_sample_count field this reuses.
#define ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION 0.05f

#define ADAPTIVE_TUNE_NVS_PARTITION "kiln_nvs"
#define ADAPTIVE_TUNE_NVS_NAMESPACE "adap_tune"
#define ADAPTIVE_TUNE_NVS_KEY_ENMASK "en_mask"

// sum(u*u) below this is too little energy to divide by -- a handful of
// near-zero-duty observations (which ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION
// should already have excluded one at a time, but a stacked ring of several
// small-but-individually-legal duties could still sum small) would otherwise
// produce an enormous, meaningless K from dividing by almost nothing.
#define ADAPTIVE_TUNE_FIT_MIN_DENOM 1e-4

// ---------------------------------------------------------------------
// Full coupled identification (Layer 2) -- guards.
// ---------------------------------------------------------------------

// Joint (all-zone) dwell observations, shared module-wide (one ring, not
// per-zone -- a "joint observation" IS every zone's duty/rise at once).
// 24 rows * MAX31856_CHANNEL_COUNT(3) zones * 2 floats * 4 bytes = 576
// bytes -- same "small enough that PSRAM buys nothing" reasoning as the
// per-zone ring above, kept as a plain static array for the same reason.
#define ADAPTIVE_TUNE_JOINT_RING_CAPACITY 24

// n unknowns per affected-zone row (one coupling_coeff[i][j] per neighbour
// j, n total including the diagonal j==i) need at least n equations to be
// determined at all; PID_EXPANSION_PLAN.md's own worked example (3 zones ->
// 9 unknowns, one dwell -> 3 equations) is exactly this n-unknowns-per-row
// framing, not "9 equations needed from one dwell" -- each affected zone's
// row is its own n-unknown system, solved separately, sharing only the
// duty design matrix. This margin (independent observations REQUIRED beyond
// the bare minimum n) mirrors the diagonal fit's own "4 required vs 2
// theoretical minimum" choice above -- degrees of freedom for the
// least-squares residual to mean anything, not just be exactly zero.
#define ADAPTIVE_TUNE_COUPLED_OBS_MARGIN 2u

// A joint row is committed only when every zone's duty clears this floor --
// same reasoning and same numeric value as ADAPTIVE_TUNE_MIN_DUTY_FOR_
// OBSERVATION, applied to every column of the row, not just the affected
// zone's own duty: an unexcited neighbour (duty ~ 0) contributes a
// degenerate (all-zero) column to the design matrix, which is exactly what
// the conditioning check below exists to refuse -- excluding it up front is
// cheaper and gives a more specific refusal reason.
#define ADAPTIVE_TUNE_JOINT_MIN_DUTY ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION

// Per-run bound on how far a single coupling_coeff[i][j] cell may move.
// Additive, not a ratio like ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE -- a coupling
// cell legitimately starts at 0.0 ("never measured"), where any ratio-based
// cap is either 0 (never learns anything) or infinite (no cap at all).
// 6.0 is deliberately BELOW ALPHA * ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS
// (0.15 * 50 = 7.5) so it is actually reachable from a near-zero prior, not
// just a decorative number that the near-zero branch's own absolute cap
// (50.0) already makes unreachable at any value >= 7.5 -- see the D3 note in
// the review this fixes (an earlier 10.0 here silently could ONLY ever bind
// in the confident-prior regime below, at prior >= ~10.0, which is exactly
// what this guard's original comment claimed it was NOT for). At 6.0 it
// binds in BOTH regimes it is meant to cover: a near-zero-prior fit close to
// the 50.0 implausibility ceiling (prior ~0, fit > ~40), and a confident
// prior's fit near its own 5x ratio ceiling (prior >~10.0, see
// try_refine_coupled_locked()'s combined ratio/absolute guard).
#define ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE 6.0f

// Same blend fraction as the diagonal path -- see ADAPTIVE_TUNE_BLEND_ALPHA.
#define ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA ADAPTIVE_TUNE_BLEND_ALPHA

// Implausible-fit guard for a cell whose prior is (near) zero, where the
// diagonal path's ratio-based jump guard has no denominator to divide by.
// 50.0 is half of ZONE_COUPLING_COEFF_MAX -- a raw fit above this, against a
// never-measured prior, is refused outright rather than blended-then-capped
// (same "refuse a contaminated point rather than quietly shrink it" posture
// as ADAPTIVE_TUNE_MAX_JUMP_RATIO).
#define ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS 50.0f
#define ADAPTIVE_TUNE_COUPLING_PRIOR_NEAR_ZERO 1e-3f

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis from dwells -- guards.
// ---------------------------------------------------------------------

// Trailing within-dwell trace, per zone, reset at every dwell entry. 24
// samples at the hardware's ~10s logging cadence is 4 minutes -- enough for
// several cycles of any limit cycle slow enough to matter thermally (a kiln
// zone cycling faster than ~30s is not physically plausible: heater_output.h's
// HEATER_MIN_ON_MS_FLOOR alone is 10s). 24 * (4+4 bytes) * MAX31856_CHANNEL_
// COUNT(3) zones = 576 bytes -- same "small, plain static array" reasoning
// as the two rings above.
#define ADAPTIVE_TUNE_KI_TRACE_CAPACITY 24
#define ADAPTIVE_TUNE_KI_MIN_SAMPLES 12

// A temperature swing below this is at or below a MAX31856 channel's
// practical noise floor (same value quoted in this file's other comments) --
// not enough to call it a real oscillation regardless of how regular it
// looks.
#define ADAPTIVE_TUNE_KI_NOISE_FLOOR_C 0.05f

// A steady dwell error below this is unremarkable tracking, not a defect to
// diagnose -- 0.3 degC is the same order as this file's own settle-slope
// tolerance integrated over a few minutes, and well above MAX31856 noise.
#define ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C 0.3f

// OFFSET classification requires the error to be STEADY, not spiky: max
// dwell error no more than this multiple of the mean dwell error. A
// genuinely steady offset has max/mean close to 1; a value approaching 2
// already means something inside the window swung far from the mean, which
// is the OSCILLATING case's signature instead.
#define ADAPTIVE_TUNE_KI_OFFSET_MAX_OVER_MEAN 1.6f

// At least this many sign changes of (sample - window mean) before calling
// the window "oscillating" at all -- 4 crossings is 2 full swings, ruling
// out a single noisy excursion.
#define ADAPTIVE_TUNE_KI_MIN_CROSSINGS 4u

// A LIMIT_CYCLE (regular) verdict additionally requires the spacing between
// crossings to be consistent -- stddev of the gaps no more than this
// fraction of their mean. Above this, the window is oscillating but
// irregularly (hunting, not a clean limit cycle), and Ku/Tu extracted from
// it would not be trustworthy the way pid_autotune_fit_relay()'s own
// cycle-to-cycle agreement check already enforces for a real relay test.
#define ADAPTIVE_TUNE_KI_CYCLE_REGULARITY_MAX 0.5f

// Floored-integral detection: duty essentially flat (variance below this)
// AND parked within this distance of a rail (0 or 1) -- the -ff_hold floor
// pins duty at whatever P+D+ff_climb alone commands, which does not move
// tick to tick the way an unclamped integrator's output would, and in
// practice pins it low (see pid.c's own comment on the floor being reached
// on a hot-running zone). A duty trace this flat AND this close to a rail,
// co-occurring with an OFFSET-shaped error, means "the integrator cannot
// move," not "Ki is too small" -- increasing Ki would do nothing (the floor
// still applies) and the change would be silently inert at best.
#define ADAPTIVE_TUNE_KI_FLOOR_DUTY_VARIANCE 5e-4f
#define ADAPTIVE_TUNE_KI_FLOOR_DUTY_RAIL_BAND 0.05f

// Per-run bound on a Ki correction from this layer -- flat (not proportional
// to error size): a first pass on data this indirect should move a fixed,
// conservative amount and let the NEXT firing's diagnosis confirm or correct
// course. K4 fix: this constant now ALSO parameterizes adaptive_tune_
// diagnose_ki()'s raw ki_correction_pct magnitude (previously a separately
// hardcoded +-20.0f that happened to equal this constant's value, so the
// clamp below could never actually bind on any real input). See test_ki_
// diagnosis_per_run_move_is_bounded_by_configured_fraction() for the
// mutation-sensitive proof.
#define ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE 0.20f

// H3(c) (SUPERSEDED by K5): this file used to add NO cumulative bound,
// reasoning a genuinely closed loop self-limits and zones_config_set_pid()'s
// absolute ZONE_PID_GAIN_MAX (1000) ceiling was backstop enough for the
// non-closing case. K5: instrumented, a zone whose error is only PARTLY
// Ki-responsive (a load offset, a soft element, a lagging thermocouple) is
// exactly that non-closing case, and rode the 1000 ceiling to 850.6x its
// starting Ki before it finally bit -- not an OPERATIONAL bound. Fixed with
// a per-zone CUMULATIVE ceiling, expressed as a multiple of the zone's
// AUTOTUNED BASELINE Ki (adaptive_tune_zone_t.ki_baseline, latched the first
// time this layer touches the zone). 50x is well above what a genuinely
// closed loop needs (test_ki_diagnosis_converges_under_closed_loop_plant_
// feedback()'s modeled plant converges ~27x baseline) and well below the
// 850.6x runaway this bound exists to stop -- an order of magnitude sooner
// than the absolute ceiling, at a Ki an operator has some chance of
// recognizing as wrong. No comparable LOWER bound: a zone driving Ki toward
// 0 is not a runaway hazard, and the setter already refuses non-positive Ki.
#define ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT 50.0f

// Relay-style Ku/Tu extraction from a non-relay (ordinary PID) trace has no
// real hysteresis band to report -- 0 passes pid_autotune_fit_relay()'s
// sqrt(a^2-h^2) term through unmodified (h=0 reduces it to plain a).
#define ADAPTIVE_TUNE_KI_RELAY_HYSTERESIS_C 0.0f

typedef struct {
    float duty;
    float rise_c; // actual_c - ambient_c at the settled instant
} adaptive_tune_obs_t;

typedef struct {
    bool enabled;

    // Settle-window tracking, mirrors autotune's readiness_start_* fields.
    bool  dwelling_prev;
    bool  settle_start_valid;
    float settle_start_c;
    float settle_elapsed_s;
    bool  recorded_this_dwell;

    // Observation ring (oldest evicted first).
    adaptive_tune_obs_t ring[ADAPTIVE_TUNE_RING_CAPACITY];
    uint32_t ring_count;
    uint32_t ring_head; // index of the OLDEST entry
    uint32_t observations_lifetime;

    // Last-applied bookkeeping, surfaced on the zones page.
    bool     has_applied;
    float    prior_k_dc;
    float    applied_k_dc;
    float    last_delta_pct;
    uint8_t  last_applied_profile_id;
    uint32_t last_applied_unix_s;
    char     last_refusal_reason[96];

    // Within-dwell trailing trace for the Ki diagnosis, reset every dwell
    // entry, appended every dwelling tick (not gated on settle -- unlike the
    // single dc-gain point above, this needs the SHAPE of the whole window).
    float    trace_t_s[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    float    trace_actual_c[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    float    trace_duty[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    uint32_t trace_count;
    uint32_t trace_head;
    float    trace_elapsed_s;

    // Coupled-solve and Ki-apply bookkeeping, surfaced on the zones page --
    // see adaptive_tune_zone_status_t's own fields for what each means.
    uint32_t joint_observations;
    bool     coupled_attempted;
    bool     coupled_applied;
    uint8_t  coupled_cells_changed;
    char     coupled_refusal_reason[96];
    uint8_t  ki_verdict;
    float    ki_correction_pct;
    bool     ki_applied;
    char     ki_refusal_reason[96];

    // K5: this zone's autotuned baseline Ki, latched once (see
    // ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT) the first time try_refine_ki_
    // locked() reaches a live Ki, never overwritten after -- the value SIMC
    // (or a hand-tune) produced, not a rolling "most recent" value.
    bool     ki_baseline_valid;
    float    ki_baseline;
} adaptive_tune_zone_t;

static adaptive_tune_zone_t s_zones[MAX31856_CHANNEL_COUNT];

// ---------------------------------------------------------------------
// Joint (all-zone) dwell observations for the coupled solve -- module-wide,
// not per-zone, because a "joint observation" is every zone's duty/rise at
// the SAME instant. Guarded by the same s_lock as s_zones[] above.
// ---------------------------------------------------------------------

typedef struct {
    float duty[MAX31856_CHANNEL_COUNT];
    float rise_c[MAX31856_CHANNEL_COUNT]; // actual_c - ambient_c, per zone, at this joint instant
} adaptive_tune_joint_obs_t;

static adaptive_tune_joint_obs_t s_joint_ring[ADAPTIVE_TUNE_JOINT_RING_CAPACITY];
static uint32_t s_joint_ring_count;
static uint32_t s_joint_ring_head;
static uint32_t s_joint_observations_lifetime;

// True once a joint row has been committed for the CURRENT dwell -- reset
// whenever any zone begins a fresh dwell (adaptive_tune_zone_tick()'s "just
// entered this dwell" branch). Every enabled zone settles independently
// (each has its own settle_start_c/settle_elapsed_s), so with N zones
// dwelling at the same operating point, each one crossing its settle floor
// used to commit its OWN joint row from the same s_joint_last_duty/rise_c
// snapshot -- one physical dwell (one distinct operating point) silently
// contributing N near-identical rows to the ring. try_refine_coupled_
// locked()'s "N joint observations" floor is a rank/conditioning
// requirement on DISTINCT equations; duplicate rows inflate the count
// without adding one. Gating the commit on this flag makes ring rows and
// distinct dwells the same number again, so the existing floor check is
// correct without a second counter. This assumes every enabled zone shares
// the same profile segment boundaries (true for this firmware -- all zones
// in a firing follow the same profile), so "any zone enters a fresh dwell"
// is a reasonable proxy for "a new dwell has begun" module-wide.
static bool s_joint_dwell_row_committed;

// Latest known duty/rise for every zone, updated on EVERY tick for EVERY
// zone regardless of that zone's own opt-in flag -- a zone that has not
// opted its own row into learning is still a valid NEIGHBOUR column in
// another zone's coupled row, so its duty must still be tracked. Frozen
// (not reset to 0/NaN) on an invalid tick, same "hold last value" posture
// as zone_coupling_filter_tick() -- a stale value sitting unused until the
// next valid tick does no harm; only last_valid gates whether it is ever
// read.
static float s_joint_last_duty[MAX31856_CHANNEL_COUNT];
static float s_joint_last_rise_c[MAX31856_CHANNEL_COUNT];
static bool  s_joint_last_valid[MAX31856_CHANNEL_COUNT];
static SemaphoreHandle_t s_lock; // guards s_zones; taken only from this file, never across profile_executor.c's
                                  // s_exec.lock (see adaptive_tune.h's doc comment on the lock order this keeps)
static bool s_lock_ready;

static void ensure_lock(void)
{
    if (!s_lock_ready) {
        s_lock = xSemaphoreCreateMutex();
        s_lock_ready = true;
    }
}

// ---------------------------------------------------------------------
// Pure fit math -- host-tested directly, see test_adaptive_tune.c.
// ---------------------------------------------------------------------

bool adaptive_tune_fit_gain(const float *duty, const float *rise_c, uint32_t n, float *out_k)
{
    if (!duty || !rise_c || !out_k || n == 0) {
        return false;
    }
    double num = 0.0, den = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        num += (double)duty[i] * (double)rise_c[i];
        den += (double)duty[i] * (double)duty[i];
    }
    if (den < ADAPTIVE_TUNE_FIT_MIN_DENOM) {
        return false; // too little duty energy in this set to divide by
    }
    *out_k = (float)(num / den);
    return true;
}

// ---------------------------------------------------------------------
// Layer 1 -- harvest a dwell observation, one per dwell, once settled.
// ---------------------------------------------------------------------

void adaptive_tune_zone_tick(uint8_t zone_index, float actual_c, bool actual_valid, float duty, bool dwelling,
                              float ambient_c, float dt_s)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &s_zones[zone_index];

    // Joint duty/rise cache: updated for EVERY zone on EVERY tick,
    // regardless of that zone's own opt-in flag -- see s_joint_last_duty[]'s
    // own comment above. Deliberately BEFORE the enabled/actual_valid/
    // dwelling gates below: an opted-out zone, or one whose own reading is
    // bad this instant, must still contribute its latest-known duty as a
    // neighbour column for another zone's coupled row.
    if (actual_valid && !isnan(ambient_c) && isfinite(actual_c) && isfinite(ambient_c)) {
        s_joint_last_duty[zone_index] = duty;
        s_joint_last_rise_c[zone_index] = actual_c - ambient_c;
        s_joint_last_valid[zone_index] = true;
    } else {
        s_joint_last_valid[zone_index] = false;
    }

    // F3 fix: dwell-transition bookkeeping (dwelling_prev, and everything
    // that resets on a fresh dwell entry) runs UNCONDITIONALLY here, before
    // the enabled/actual_valid/ambient gates below -- same "before the
    // gates" posture as the joint cache update above, and for the same
    // reason. Previously dwelling_prev was only ever touched inside the
    // gated branches, which left two holes (see the review's F3 finding):
    //   (a) a zone whose first dwelling tick(s) have !actual_valid or a NaN
    //       ambient hit the early return below without ever setting
    //       dwelling_prev true. When a later tick in the SAME dwell finally
    //       had good data, dwelling_prev was still false, so THAT tick was
    //       misread as "just entered this dwell" -- re-clearing
    //       s_joint_dwell_row_committed and admitting a second joint row
    //       from one physical dwell.
    //   (b) identically for a zone that is enabled mid-dwell: every tick
    //       before it was enabled returned early (via the old !z->enabled
    //       check) without updating dwelling_prev, so the first tick after
    //       enabling hit the same false "fresh dwell" branch.
    // Tracking the raw dwelling transition here, independent of data
    // validity and the opt-in flag, makes "just entered this dwell" true
    // exactly once per physical dwell, regardless of what the gates below
    // do with any given tick's data.
    bool dwell_just_entered = dwelling && !z->dwelling_prev;
    z->dwelling_prev = dwelling;

    if (dwell_just_entered) {
        // Just entered this dwell -- start a fresh Ki-diagnosis trace (see
        // adaptive_tune_zone_t's own comment on trace_t_s[] -- this window
        // covers exactly one dwell, unlike the K_dc observation ring, which
        // spans the whole run) and allow one more joint row to be committed
        // for it (see s_joint_dwell_row_committed's own comment: this zone
        // starting a fresh dwell is this module's proxy for "a new joint
        // dwell has begun" module-wide). The settle window itself is
        // (re)started below, at the first tick that actually has usable
        // data -- which may be this same tick, or a later one if this one's
        // reading is not trustworthy (see holes (a)/(b) above).
        z->settle_start_valid = false;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
        z->trace_count = 0;
        z->trace_head = 0;
        z->trace_elapsed_s = 0.0f;
        // H4(c): this flag is shared MODULE-WIDE (see its own comment), so
        // ANY zone's dwell_just_entered -- including a DISABLED zone's, since
        // this branch runs unconditionally, before the z->enabled gate below
        // -- clears it and reopens the commit window. Safe today only
        // because profile_executor.c's s_exec.dwelling is a single global
        // flag applied to every zone in the same loop iteration (see
        // profile_executor.c:501), so every enabled AND disabled zone
        // transitions dwelling->true on the exact same tick; a disabled
        // zone's "fresh dwell" is therefore never actually early relative to
        // the enabled zones it shares a physical dwell with. If profile_
        // executor.c ever moves to a PER-ZONE dwelling signal, a disabled
        // zone could enter its own dwell on a different tick than the
        // enabled zones and reopen this shared commit flag mid-dwell,
        // silently admitting a second joint row from what the enabled zones
        // still consider one physical dwell.
        s_joint_dwell_row_committed = false;
    }
    if (!dwelling) {
        // Not dwelling any more (or not yet) -- keep the settle tracker
        // clean so the next dwell starts from a baseline uncontaminated by
        // this tick, whatever z->enabled/actual_valid say about it.
        z->settle_start_valid = false;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
    }

    if (!z->enabled || !actual_valid || isnan(ambient_c)) {
        // Learning is off, or this tick has nothing trustworthy to offer.
        // dwelling_prev and the fresh-dwell reset above already ran
        // unconditionally, so there is nothing left to do here.
        xSemaphoreGive(s_lock);
        return;
    }

    if (!dwelling) {
        xSemaphoreGive(s_lock);
        return;
    }

    if (!z->settle_start_valid) {
        // Either this is genuinely the first valid tick of a fresh dwell, or
        // the dwell was entered earlier while the data was invalid/the zone
        // was disabled (holes (a)/(b) above) -- either way, this is the
        // first valid opportunity to start the settle window for this
        // dwell.
        z->settle_start_valid = true;
        z->settle_start_c = actual_c;
    }
    z->settle_elapsed_s += dt_s;

    // Trace append happens on EVERY dwelling tick, not gated on settle --
    // the Ki diagnosis needs the window's SHAPE (drift, oscillation), which
    // the single post-settle dc-gain point below cannot provide.
    z->trace_elapsed_s += dt_s;
    {
        uint32_t tslot = (z->trace_head + z->trace_count) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
        if (z->trace_count < ADAPTIVE_TUNE_KI_TRACE_CAPACITY) {
            z->trace_count++;
        } else {
            z->trace_head = (z->trace_head + 1) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
        }
        z->trace_t_s[tslot] = z->trace_elapsed_s;
        z->trace_actual_c[tslot] = actual_c;
        z->trace_duty[tslot] = duty;
    }

    if (z->recorded_this_dwell || !z->settle_start_valid) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (z->settle_elapsed_s < ADAPTIVE_TUNE_SETTLE_MIN_S) {
        xSemaphoreGive(s_lock);
        return; // still within the settle window -- keep waiting
    }
    float slope = fabsf(actual_c - z->settle_start_c) / z->settle_elapsed_s;
    if (slope > ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S) {
        // Genuinely still drifting -- do NOT reset the window; a real drift
        // keeps failing this test every tick going forward (the elapsed-time
        // denominator only grows), which is the correct outcome. A brief
        // noise spike self-corrects on the next tick's smaller slope.
        xSemaphoreGive(s_lock);
        return;
    }

    // Settled. One observation per dwell, regardless of outcome below --
    // recorded_this_dwell is set on every path out from here so a marginal
    // (too-low-duty, implausible) dwell does not get re-evaluated every
    // tick for the rest of its length.
    z->recorded_this_dwell = true;

    if (duty < ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION) {
        xSemaphoreGive(s_lock);
        return; // real steady state, but too little duty to trust the ratio
    }
    float rise_c = actual_c - ambient_c;
    if (!isfinite(rise_c) || rise_c <= 0.0f) {
        // A settled dwell above ambient always has positive rise on a
        // working heater; zero/negative here means a bad ambient capture
        // or a zone that never actually rose (thermocouple/relay fault
        // elsewhere) -- not a physically usable point either way.
        xSemaphoreGive(s_lock);
        return;
    }

    uint32_t slot = (z->ring_head + z->ring_count) % ADAPTIVE_TUNE_RING_CAPACITY;
    if (z->ring_count < ADAPTIVE_TUNE_RING_CAPACITY) {
        z->ring_count++;
    } else {
        z->ring_head = (z->ring_head + 1) % ADAPTIVE_TUNE_RING_CAPACITY; // evict oldest
    }
    z->ring[slot].duty = duty;
    z->ring[slot].rise_c = rise_c;
    z->observations_lifetime++;

    // Joint (all-zone) row for the coupled solve, committed at the SAME
    // settle instant as this zone's own diagonal point above -- only if
    // EVERY zone (this one included) currently has a fresh, valid,
    // above-floor duty reading. A partial row (a neighbour never having
    // ticked yet, or sitting invalid/too-low this instant) is discarded
    // outright rather than committed with a placeholder -- a zero-filled
    // column would silently poison that zone out of every future row's
    // design matrix instead of just being absent from this one.
    {
        bool joint_ok = !s_joint_dwell_row_committed; // see s_joint_dwell_row_committed's own comment --
                                                       // at most one joint row per distinct dwell
        for (uint8_t j = 0; joint_ok && j < MAX31856_CHANNEL_COUNT; j++) {
            if (!s_joint_last_valid[j] || s_joint_last_duty[j] < ADAPTIVE_TUNE_JOINT_MIN_DUTY) {
                joint_ok = false;
            }
        }
        if (joint_ok) {
            s_joint_dwell_row_committed = true;
            uint32_t jslot = (s_joint_ring_head + s_joint_ring_count) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
            if (s_joint_ring_count < ADAPTIVE_TUNE_JOINT_RING_CAPACITY) {
                s_joint_ring_count++;
            } else {
                s_joint_ring_head = (s_joint_ring_head + 1) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
            }
            memcpy(s_joint_ring[jslot].duty, s_joint_last_duty, sizeof(s_joint_last_duty));
            memcpy(s_joint_ring[jslot].rise_c, s_joint_last_rise_c, sizeof(s_joint_last_rise_c));
            s_joint_observations_lifetime++;
        }
    }

    // The Ki diagnosis itself runs at adaptive_tune_run_end(), not here --
    // it needs profile_exec_firing_stats_t's dwell_err_mean_c/dwell_err_max_c
    // (the SETPOINT-aware error figures profile_executor.c already computes;
    // this file never receives setpoint_c per tick, see adaptive_tune.c's
    // top comment), which only arrive with the run record. z->trace_* above
    // is left as-is (this dwell's most recent trace) for that call to read.

    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------
// Layer 2/3 -- batch fit, blend, guard, and apply -- called only from
// adaptive_tune_run_end(), i.e. only ever at a run boundary. This is what
// makes a mid-firing bump structurally impossible: there is no other call
// site that can reach zones_config_set_model()/set_pid() from this file.
// ---------------------------------------------------------------------

static void set_refusal(adaptive_tune_zone_t *z, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z->last_refusal_reason, sizeof(z->last_refusal_reason), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "refined not applied: %s", z->last_refusal_reason);
}

// Returns true only on the happy path where zones_config_set_model()/
// set_pid() actually ran -- i.e. Kp/Ki/Kd were just rewritten this run from
// the SIMC recompute. adaptive_tune_run_end() uses this to decide whether
// try_refine_ki_locked() may run this same run -- see that call site's own
// comment (D5).
static bool try_refine_zone_locked(uint8_t zi, uint8_t profile_id)
{
    adaptive_tune_zone_t *z = &s_zones[zi];

    if (z->ring_count < ADAPTIVE_TUNE_MIN_OBSERVATIONS) {
        set_refusal(z, "only %u/%u dwell observations", (unsigned)z->ring_count,
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
        set_refusal(z, "observations too clustered (duty spread %.3f < %.3f)", (double)(umax - umin),
                    (double)ADAPTIVE_TUNE_MIN_DUTY_SPREAD);
        return false;
    }

    float k_fit;
    if (!adaptive_tune_fit_gain(duty, rise, z->ring_count, &k_fit)) {
        set_refusal(z, "fit degenerate (insufficient duty energy)");
        return false;
    }
    if (!(k_fit > 0.0f)) {
        set_refusal(z, "fitted gain %.4f is not positive", (double)k_fit);
        return false;
    }

    float k_dc, tau_s, dead_time_s;
    if (!zones_config_get_model(zi, &k_dc, &tau_s, &dead_time_s) || !(k_dc > 0.0f)) {
        set_refusal(z, "no existing step-test model -- learning refines, it does not create one");
        return false;
    }

    if (k_fit > k_dc * ADAPTIVE_TUNE_MAX_JUMP_RATIO || k_fit < k_dc / ADAPTIVE_TUNE_MAX_JUMP_RATIO) {
        set_refusal(z, "fit %.4f is implausible against prior K %.4f (>%.0fx)", (double)k_fit, (double)k_dc,
                    (double)ADAPTIVE_TUNE_MAX_JUMP_RATIO);
        return false;
    }

    float k_blended = k_dc + ADAPTIVE_TUNE_BLEND_ALPHA * (k_fit - k_dc);
    float max_move = k_dc * ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE;
    if (k_blended > k_dc + max_move) k_blended = k_dc + max_move;
    if (k_blended < k_dc - max_move) k_blended = k_dc - max_move;
    if (!(k_blended > 0.0f)) {
        set_refusal(z, "blended gain %.4f is not positive", (double)k_blended);
        return false;
    }

    // F1: refuse rather than write a blend too small to be a material
    // change -- see ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC's own comment.
    float material_move = fabsf(k_blended - k_dc);
    if (material_move < k_dc * ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC) {
        set_refusal(z, "blended gain %.4f is not a material change from prior %.4f (<%.2f%%)", (double)k_blended,
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
        set_refusal(z, "SIMC refused the refined model: %s", gains.refusal_reason);
        return false;
    }

    if (!zones_config_set_model(zi, k_blended, tau_s, dead_time_s)) {
        set_refusal(z, "zones_config_set_model() rejected %.4f/%.1f/%.1f", (double)k_blended, (double)tau_s,
                    (double)dead_time_s);
        return false;
    }
    if (!zones_config_set_pid(zi, gains.kp, gains.ki, gains.kd)) {
        set_refusal(z, "zones_config_set_pid() rejected %.4f/%.4f/%.4f", (double)gains.kp, (double)gains.ki,
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

    ESP_LOGI(TAG, "zone %u: K_dc %.4f -> %.4f (%.1f%%) from %u observations, profile %u", (unsigned)zi,
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

static void set_reason(char *buf, size_t bufsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, bufsz, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
}

static void try_refine_coupled_locked(uint8_t zi)
{
    adaptive_tune_zone_t *z = &s_zones[zi];
    z->coupled_attempted = true;
    z->coupled_applied = false;
    z->coupled_cells_changed = 0;
    z->joint_observations = s_joint_ring_count;

    const uint8_t n = MAX31856_CHANNEL_COUNT;
    uint32_t min_obs = (uint32_t)n + ADAPTIVE_TUNE_COUPLED_OBS_MARGIN;
    if (s_joint_ring_count < min_obs) {
        set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
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
        set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "joint observation set degenerate for a determined solve");
        return;
    }
    if (r == ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED) {
        set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "joint duty matrix ill-conditioned (cond above ~1e4, zone_coupling_solve.h's own pivot floor)");
        return;
    }

    float prior_row[MAX31856_CHANNEL_COUNT];
    if (!zones_config_get_coupling(zi, prior_row)) {
        set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
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
        ESP_LOGI(TAG, "zone %u: coupled solve refined %u coupling cell(s) from %u joint observations",
                 (unsigned)zi, (unsigned)changed, (unsigned)s_joint_ring_count);
    } else {
        set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason),
                   "coupled solve succeeded but every off-diagonal cell was implausible or rejected");
    }
}

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis -- pure classification (host-tested directly)
// plus its locked apply helper.
// ---------------------------------------------------------------------

bool adaptive_tune_diagnose_ki(const float *actual_c, const float *duty, uint32_t n, float dt_s,
                               float dwell_err_mean_c, float dwell_err_max_c, adaptive_tune_ki_diag_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!actual_c || !duty || n < ADAPTIVE_TUNE_KI_MIN_SAMPLES || !(dt_s > 0.0f)) {
        out->verdict = ADAPTIVE_TUNE_KI_INSUFFICIENT;
        return true;
    }

    double sum = 0.0;
    float amin = INFINITY, amax = -INFINITY;
    float dmin = INFINITY, dmax = -INFINITY;
    double dsum = 0.0;
    for (uint32_t k = 0; k < n; k++) {
        sum += (double)actual_c[k];
        if (actual_c[k] < amin) amin = actual_c[k];
        if (actual_c[k] > amax) amax = actual_c[k];
        dsum += (double)duty[k];
        if (duty[k] < dmin) dmin = duty[k];
        if (duty[k] > dmax) dmax = duty[k];
    }
    float mean = (float)(sum / (double)n);
    float dmean = (float)(dsum / (double)n);
    double dvar = 0.0;
    for (uint32_t k = 0; k < n; k++) {
        double d = (double)duty[k] - (double)dmean;
        dvar += d * d;
    }
    dvar /= (double)n;
    float amplitude = (amax - amin) / 2.0f;
    float duty_amp = (dmax - dmin) / 2.0f;

    // Zero crossings of (actual_c - mean), plus their (fractional) sample
    // index, for the regularity/period check below.
    float cross_idx[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    uint32_t ncross = 0;
    for (uint32_t k = 1; k < n; k++) {
        float p0 = actual_c[k - 1] - mean;
        float p1 = actual_c[k] - mean;
        if ((p0 < 0.0f && p1 >= 0.0f) || (p0 > 0.0f && p1 <= 0.0f)) {
            if (ncross < ADAPTIVE_TUNE_KI_TRACE_CAPACITY) {
                cross_idx[ncross] = (float)k;
            }
            ncross++;
        }
    }
    out->zero_crossings = ncross;

    bool regular = false;
    float mean_gap = 0.0f;
    if (ncross >= 3 && ncross <= ADAPTIVE_TUNE_KI_TRACE_CAPACITY) {
        uint32_t ngaps = ncross - 1;
        double gsum = 0.0;
        for (uint32_t g = 0; g < ngaps; g++) gsum += (double)(cross_idx[g + 1] - cross_idx[g]);
        mean_gap = (float)(gsum / (double)ngaps);
        double gvar = 0.0;
        for (uint32_t g = 0; g < ngaps; g++) {
            double d = (double)(cross_idx[g + 1] - cross_idx[g]) - (double)mean_gap;
            gvar += d * d;
        }
        gvar /= (double)ngaps;
        float gstd = (float)sqrt(gvar);
        if (mean_gap > 0.0f && (gstd / mean_gap) <= ADAPTIVE_TUNE_KI_CYCLE_REGULARITY_MAX) {
            regular = true;
        }
    }

    // A regular, multi-crossing oscillation is the ONLY evidence this
    // function trusts for "Ki too large" -- see below for why the half-
    // window drift figure that used to sit here was removed rather than
    // fixed in place.
    if (amplitude > ADAPTIVE_TUNE_KI_NOISE_FLOOR_C && ncross >= ADAPTIVE_TUNE_KI_MIN_CROSSINGS) {
        if (regular) {
            out->verdict = ADAPTIVE_TUNE_KI_LIMIT_CYCLE;
            out->tu_estimate_s = 2.0f * mean_gap * dt_s; // consecutive crossings are ~half a period apart
            const float pi = 3.14159265358979f;
            float denom = pi * amplitude; // relay hysteresis h == 0 for a non-relay trace, see
                                           // ADAPTIVE_TUNE_KI_RELAY_HYSTERESIS_C
            out->ku_estimate = (denom > 1e-6f) ? (4.0f * duty_amp / denom) : 0.0f;
            // K4: derived from the per-run-cap constant, not a separately
            // hardcoded literal -- see that constant's own comment.
            out->ki_correction_pct = -(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f);
        } else {
            out->verdict = ADAPTIVE_TUNE_KI_OSCILLATING; // hunting -- irregular
            out->ki_correction_pct = -(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f);
        }
        return true;
    }

    // Floored-duty check FIRST, ahead of the offset test below and
    // unconditional on dwell_err_mean_c -- floored means NO correction,
    // always, full stop; it must never be reachable only through the
    // offset branch's own threshold gate (a floored zone whose steady
    // error happens to sit right at ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C's
    // edge, or that gets shadowed by some earlier branch, must not slip
    // through with a Ki change).
    bool floored = ((float)dvar < ADAPTIVE_TUNE_KI_FLOOR_DUTY_VARIANCE) &&
                   (dmean < ADAPTIVE_TUNE_KI_FLOOR_DUTY_RAIL_BAND ||
                    dmean > 1.0f - ADAPTIVE_TUNE_KI_FLOOR_DUTY_RAIL_BAND);
    if (floored) {
        // Duty is pinned near a rail and essentially not moving -- the
        // classic -ff_hold floor signature (pid.c), NOT a small-Ki
        // signature, regardless of what the temperature trace or the
        // dwell error figures look like. See adaptive_tune_ki_verdict_t's
        // own doc comment. Deliberately NO correction: raising Ki here
        // would be inert (the floor still applies) at best.
        out->verdict = ADAPTIVE_TUNE_KI_FLOORED;
        out->ki_correction_pct = 0.0f;
        return true;
    }

    // A former branch here flagged any first-half-vs-second-half mean shift
    // above a fixed threshold (the now-removed ADAPTIVE_TUNE_KI_DRIFT_
    // THRESHOLD_C) as "OSCILLATING" (Ki too large) with fabsf() applied to
    // the shift. That is sign-blind: this
    // function is never handed the setpoint (see this file's top comment),
    // so it cannot tell a slow monotonic APPROACH to setpoint (still
    // settling -- the classic too-small-Ki signature) from a slow walk AWAY
    // from it, and a one-directional trend crosses its own window mean only
    // once, which is not oscillation evidence by any definition -- the
    // crossing/regularity branch above is what identifies a limit cycle,
    // not an unsigned half-window shift. Rather than guess a direction from
    // data that cannot support the guess, this function now makes NO
    // separate drift-based call: a window that fails the crossing test
    // above falls through to the offset/floored evaluation, which uses
    // dwell_err_mean_c/dwell_err_max_c -- the caller's own SIGNED-magnitude,
    // whole-dwell error figures -- and always corrects in the direction
    // that is actually justified by unsigned evidence (increase Ki for a
    // steady, non-floored offset; nothing otherwise).
    if (dwell_err_mean_c > ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C &&
        dwell_err_max_c <= dwell_err_mean_c * ADAPTIVE_TUNE_KI_OFFSET_MAX_OVER_MEAN) {
        out->verdict = ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL;
        out->ki_correction_pct = ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f; // K4, see comment above
        return true;
    }

    out->verdict = ADAPTIVE_TUNE_KI_OK;
    return true;
}

static void try_refine_ki_locked(uint8_t zi, const profile_exec_firing_stats_t *stats)
{
    adaptive_tune_zone_t *z = &s_zones[zi];
    z->ki_applied = false;

    if (z->trace_count < ADAPTIVE_TUNE_KI_MIN_SAMPLES) {
        z->ki_verdict = (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT;
        z->ki_correction_pct = 0.0f;
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "only %u/%u within-dwell trace samples", (unsigned)z->trace_count,
                   (unsigned)ADAPTIVE_TUNE_KI_MIN_SAMPLES);
        return;
    }

    float abuf[ADAPTIVE_TUNE_KI_TRACE_CAPACITY], dbuf[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    for (uint32_t i = 0; i < z->trace_count; i++) {
        uint32_t idx = (z->trace_head + i) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
        abuf[i] = z->trace_actual_c[idx];
        dbuf[i] = z->trace_duty[idx];
    }
    uint32_t first_idx = z->trace_head;
    uint32_t last_idx = (z->trace_head + z->trace_count - 1) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
    float span_s = z->trace_t_s[last_idx] - z->trace_t_s[first_idx];
    float dt_est = (z->trace_count > 1) ? (span_s / (float)(z->trace_count - 1)) : 0.0f;

    adaptive_tune_ki_diag_t diag;
    adaptive_tune_diagnose_ki(abuf, dbuf, z->trace_count, dt_est, stats->dwell_err_mean_c, stats->dwell_err_max_c,
                              &diag);
    z->ki_verdict = (uint8_t)diag.verdict;
    z->ki_correction_pct = diag.ki_correction_pct;

    if (diag.verdict == ADAPTIVE_TUNE_KI_OK || diag.verdict == ADAPTIVE_TUNE_KI_INSUFFICIENT) {
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "no Ki correction indicated (verdict %u)",
                   (unsigned)diag.verdict);
        return;
    }
    if (diag.verdict == ADAPTIVE_TUNE_KI_FLOORED) {
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "offset matches the -ff_hold integral floor signature, not small Ki -- withholding correction");
        return;
    }

    float kp, ki, kd;
    if (!zones_config_get_pid(zi, &kp, &ki, &kd) || !(ki > 0.0f)) {
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "no existing positive Ki to refine");
        return;
    }

    // K5: latch this zone's autotuned baseline the first time this layer
    // reaches a live Ki for it -- see ki_baseline's struct comment.
    if (!z->ki_baseline_valid) {
        z->ki_baseline = ki;
        z->ki_baseline_valid = true;
    }

    float cap_pct = ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f;
    float capped_pct = diag.ki_correction_pct;
    if (capped_pct > cap_pct) capped_pct = cap_pct;
    if (capped_pct < -cap_pct) capped_pct = -cap_pct;
    float new_ki = ki * (1.0f + capped_pct / 100.0f);
    if (!(new_ki > 0.0f) || !isfinite(new_ki)) {
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "corrected Ki %.5f is not a valid gain",
                   (double)new_ki);
        return;
    }

    // K5: the operational bound this layer is accountable for -- see
    // ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT. Checked BEFORE the setter call
    // so a bound refusal is reported as such, not as an ordinary rejection.
    float cumulative_ceiling = z->ki_baseline * ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT;
    if (new_ki > cumulative_ceiling) {
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "corrected Ki %.5f exceeds the cumulative bound (%.1fx autotuned baseline %.5f = %.5f) -- "
                   "withholding further growth",
                   (double)new_ki, (double)ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT, (double)z->ki_baseline,
                   (double)cumulative_ceiling);
        return;
    }

    if (!zones_config_set_pid(zi, kp, new_ki, kd)) {
        set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "zones_config_set_pid() rejected the corrected Ki");
        return;
    }

    z->ki_applied = true;
    z->ki_refusal_reason[0] = '\0';
    ESP_LOGI(TAG, "zone %u: Ki %.5f -> %.5f (%.1f%%, verdict %u) from dwell trace diagnosis", (unsigned)zi,
             (double)ki, (double)new_ki, (double)capped_pct, (unsigned)diag.verdict);
}

// H2/K1/K2 fix: every run_end path that skips both try_refine_zone_locked()
// and try_refine_ki_locked()/try_refine_coupled_locked() entirely must reset
// this run's PER-RUN status fields (ki_applied/ki_verdict/ki_correction_pct/
// coupled_applied/coupled_cells_changed) to a neutral "nothing happened this
// run" state, not just write a refusal-reason STRING -- adaptive_tune_get_
// status() publishes all five verbatim, so a path that only touched the
// strings left them holding whatever a PREVIOUS run left there (measured
// pre-H2: a faulted run right next to a stale "Ki correction applied,
// +20%" from the prior run).
//
// K1: H2's own fix still missed two more paths of the SAME shape -- the
// `!z->enabled` and `!zr->active` (the profile's zone mask) continues. On a
// kiln with fewer zones wired than MAX31856_CHANNEL_COUNT, ANY profile that
// doesn't touch a given zone left it publishing its previous firing's status
// forever. Restructured so there is exactly ONE call site that can skip a
// zone, via an if/else-if chain rather than a sequence of early `continue`s
// -- a future fifth skip condition is just another else-if into the SAME
// reset call, not a new early-exit that has to remember to make it.
//
// K2: has_applied is documented (adaptive_tune.h) as a LIFETIME LATCH --
// zones_page.html gates its "Last applied change" column on it, alongside
// prior_k_dc/applied_k_dc/last_delta_pct/last_applied_profile_id, none of
// which are per-run either. H2's fix cleared has_applied on every skip path,
// a REGRESSION: a faulted run after a real applied refinement made the UI
// revert to "no refinement applied yet" even though the applied change is
// still exactly what is live on the zone. reset_run_status_locked() below
// therefore leaves has_applied and its lifetime siblings untouched -- only
// the genuinely PER-RUN fields (coupled_*, ki_*) are reset here.
static void reset_run_status_locked(adaptive_tune_zone_t *z, const char *reason)
{
    set_refusal(z, "%s", reason);
    set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason), "%s", reason);
    z->coupled_attempted = false;
    z->coupled_applied = false;
    z->coupled_cells_changed = 0;
    set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "%s", reason);
    z->ki_applied = false;
    z->ki_verdict = (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT;
    z->ki_correction_pct = 0.0f;
}

void adaptive_tune_run_end(const profile_firing_run_record_t *rec, bool clean)
{
    if (!rec) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_t *z = &s_zones[zi];
        const profile_firing_zone_record_t *zr = &rec->zones[zi];

        // K1: single skip decision for this zone -- see block comment above.
        const char *skip_reason = NULL;
        char excluded_reason_buf[96]; // must outlive the chain below -- so skip_reason never dangles
        if (!z->enabled) {
            skip_reason = "zone not opted into adaptive tuning -- not used as training data";
        } else if (!zr->active) {
            skip_reason = "zone not active in this profile's zone mask -- not used as training data";
        } else if (!clean) {
            skip_reason = "run was faulted or stopped early -- not used as training data";
        } else {
            uint32_t total = zr->stats.sample_count + zr->stats.excluded_sample_count;
            if (total > 0 &&
                (float)zr->stats.excluded_sample_count / (float)total > ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION) {
                snprintf(excluded_reason_buf, sizeof(excluded_reason_buf),
                          "run excluded %u/%u samples (>%.0f%%) -- not used as training data",
                          (unsigned)zr->stats.excluded_sample_count, (unsigned)total,
                          (double)(ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION * 100.0f));
                skip_reason = excluded_reason_buf;
            }
        }
        if (skip_reason) {
            reset_run_status_locked(z, skip_reason);
            continue;
        }
        // D5: try_refine_zone_locked() rewrites Kp/Ki/Kd from a fresh SIMC
        // recompute when it applies. try_refine_ki_locked() diagnoses Ki
        // from this run's WITHIN-DWELL TRACE -- evidence gathered under
        // whatever Ki was actually running during the dwell, which is the
        // OLD value if the model refine just replaced it. Running the Ki
        // diagnosis's correction on top of a Ki that postdates the evidence
        // it was measured against is exactly the blind-stacking bug this
        // fixes: the two layers do not compose in one run, so only one of
        // them may act. The model/K_dc refinement wins when both would
        // apply -- it is the more direct measurement (a dwell duty/rise
        // ratio) versus the Ki diagnosis's shape-based inference, and a
        // freshly-recomputed SIMC Ki is itself already responsive to a
        // gain change this run. The Ki diagnosis gets its turn on any run
        // where the model refine did not fire (guard refusal, no change,
        // or the zone's coupled/diagonal fit was simply not due) -- by
        // which point its trace evidence and the live Ki agree on which
        // run produced them.
        bool model_refined = try_refine_zone_locked(zi, rec->profile_id);
        try_refine_coupled_locked(zi);
        if (!model_refined) {
            try_refine_ki_locked(zi, &zr->stats);
        } else {
            // F2: unlike the full-skip paths above (reset_run_status_
            // locked()), coupled_* is NOT reset here -- the
            // model refine and coupled solve DID run this cycle and their
            // fields legitimately reflect this run's own outcome. Only the
            // Ki-diagnosis fields are stale here (that diagnosis alone was
            // skipped), so only those three are reset.
            //
            // The diagnosis did not run this cycle, so ki_verdict/
            // ki_correction_pct must not keep publishing whatever they held
            // from a PREVIOUS run -- adaptive_tune_get_status() surfaces
            // both verbatim, and a stale verdict/pct read as this run's
            // result is exactly the kind of "reset one side" hole this
            // module has shipped before. ADAPTIVE_TUNE_KI_INSUFFICIENT is
            // the closest existing verdict for "nothing to report" (see its
            // own doc comment); there is no dedicated "skipped" verdict, so
            // the refusal reason string is what actually distinguishes this
            // case from a real too-short-trace refusal.
            z->ki_applied = false;
            z->ki_verdict = (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT;
            z->ki_correction_pct = 0.0f;
            set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                       "Ki diagnosis skipped this run -- the model/PID refinement already rewrote Ki from SIMC, "
                       "see adaptive_tune_run_end()'s D5 comment");
        }
    }
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------
// Opt-in flag: own NVS namespace, flash-worker-routed write.
// ---------------------------------------------------------------------

typedef struct {
    uint8_t mask;
    esp_err_t result;
} enmask_job_t;

static void save_enmask_job(void *arg)
{
    enmask_job_t *job = (enmask_job_t *)arg;
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition(ADAPTIVE_TUNE_NVS_PARTITION, ADAPTIVE_TUNE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        job->result = err;
        return;
    }
    err = nvs_set_u8(h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, job->mask);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    job->result = err;
}

static uint8_t enmask_locked(void)
{
    uint8_t mask = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (s_zones[zi].enabled) {
            mask |= (uint8_t)(1u << zi);
        }
    }
    return mask;
}

bool adaptive_tune_set_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_zones[zone_index].enabled = enabled;
    enmask_job_t job = {.mask = enmask_locked(), .result = ESP_FAIL};
    xSemaphoreGive(s_lock);

    // Dispatched OUTSIDE the lock -- uart_bridge_ext_run_on_flash_worker()
    // blocks the calling task until the worker task runs the job (see that
    // function's own doc comment), and this file's lock must not be held
    // across a wait on a different task.
    esp_err_t err = uart_bridge_ext_run_on_flash_worker(save_enmask_job, &job);
    if (err != ESP_OK || job.result != ESP_OK) {
        ESP_LOGE(TAG, "adaptive_tune_set_enabled(%u,%d): NVS save failed: %s / %s", (unsigned)zone_index,
                 (int)enabled, esp_err_to_name(err), esp_err_to_name(job.result));
        return false; // live flag still stands -- see time_sync_set_tz()'s identical convention
    }
    return true;
}

bool adaptive_tune_get_enabled(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool en = s_zones[zone_index].enabled;
    xSemaphoreGive(s_lock);
    return en;
}

void adaptive_tune_get_status(uint8_t zone_index, adaptive_tune_zone_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &s_zones[zone_index];
    out->enabled = z->enabled;
    out->ring_count = z->ring_count;
    out->observations_lifetime = z->observations_lifetime;
    out->has_applied = z->has_applied;
    out->prior_k_dc = z->prior_k_dc;
    out->applied_k_dc = z->applied_k_dc;
    out->last_delta_pct = z->last_delta_pct;
    out->last_applied_profile_id = z->last_applied_profile_id;
    out->last_applied_unix_s = z->last_applied_unix_s;
    strncpy(out->last_refusal_reason, z->last_refusal_reason, sizeof(out->last_refusal_reason) - 1);

    out->joint_observations = s_joint_ring_count;
    out->coupled_attempted = z->coupled_attempted;
    out->coupled_applied = z->coupled_applied;
    out->coupled_cells_changed = z->coupled_cells_changed;
    strncpy(out->coupled_refusal_reason, z->coupled_refusal_reason, sizeof(out->coupled_refusal_reason) - 1);
    out->ki_verdict = z->ki_verdict;
    out->ki_correction_pct = z->ki_correction_pct;
    out->ki_applied = z->ki_applied;
    strncpy(out->ki_refusal_reason, z->ki_refusal_reason, sizeof(out->ki_refusal_reason) - 1);
    xSemaphoreGive(s_lock);
}

// HTTP registration lives in adaptive_tune_http.c now (GET /api/adaptive_tune,
// POST /api/adaptive_tune/enable) -- split out so this file has no httpd
// dependency at all; adaptive_tune_http.c reaches everything it needs through
// the public accessors below (adaptive_tune_get_enabled/set_enabled/
// get_status). Call adaptive_tune_http_start() separately (main.c does, near
// log_http_start()) once the shared httpd server is up.

void adaptive_tune_init(void)
{
    ensure_lock();
    memset(s_zones, 0, sizeof(s_zones));

    // Boot-time read, direct (not through the flash worker -- see this
    // file's top comment on why reads are exempt).
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition(ADAPTIVE_TUNE_NVS_PARTITION, ADAPTIVE_TUNE_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t mask = 0;
        if (nvs_get_u8(h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, &mask) == ESP_OK) {
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                s_zones[zi].enabled = (mask & (1u << zi)) != 0;
            }
        }
        nvs_close(h);
    }
    // ESP_ERR_NVS_NOT_FOUND (namespace never written) leaves every zone at
    // its struct-zero default: enabled = false. DEFAULT OFF, as required.
    //
    // No httpd registration here any more -- call adaptive_tune_http_start()
    // separately once the shared httpd server is up (see adaptive_tune_http.c).
}
