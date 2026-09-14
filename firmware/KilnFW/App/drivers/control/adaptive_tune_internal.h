#ifndef ADAPTIVE_TUNE_INTERNAL_H
#define ADAPTIVE_TUNE_INTERNAL_H

/* Internal seams for the adaptive_tune.c split (2026-09-01, "files over 1500
 * lines should be broken up where it makes sense" -- adaptive_tune.c had
 * grown to 1498/1500 lines and P1's NVS-persisted Ki baseline plus P2's
 * re-justified cumulative bound would have pushed it over; profile_
 * executor.c's split (profile_executor_internal.h) is the precedent this
 * follows -- same shape: a private, non-public header carrying the structs,
 * guard constants and cross-file `static`-turned-file-scope symbols that
 * used to live in one translation unit, so the split pieces can still reach
 * each other for free.
 *
 *   adaptive_tune.c        -- lock/state ownership, Layer 1 (zone_tick, the
 *                              settle/observation/joint-cache bookkeeping),
 *                              adaptive_tune_run_end() dispatcher, opt-in
 *                              flag + Ki-baseline NVS persistence, init,
 *                              public accessors (enable/get/get_status)
 *   adaptive_tune_model.c  -- Layer 2/3 diagonal K_dc refine
 *                              (adaptive_tune_refine_zone_locked()) and the full
 *                              coupled identification (adaptive_tune_
 *                              coupled_fit(), adaptive_tune_refine_coupled_locked())
 *   adaptive_tune_ki.c     -- integral (Ki) diagnosis: the pure classifier
 *                              (adaptive_tune_diagnose_ki()) and its locked
 *                              apply helper (adaptive_tune_refine_ki_locked())
 *
 * adaptive_tune.h stays the ONLY public API; this header is not installed
 * anywhere outside the drivers/ directory and nothing outside this module's
 * three .c files (plus test_adaptive_tune.c, which #includes all three
 * directly into one TU -- same reason test_profile_executor_prestart.c does,
 * see that file's own header comment) should ever include it.
 */

#include "adaptive_tune.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "zones_config_accessors.h" /* ZONE_AUTOTUNE_K_DC_MAX -- ADAPTIVE_TUNE_K_DC_ABS_MAX below
                                       * is literally this constant, never a re-derivation of it */
#include "flash_worker.h" /* uart_bridge_ext_run_on_flash_worker() -- see
 * adaptive_tune.c's top comment for the full reasoning. Narrow header
 * (esp_err.h only), not the hand-declaration this used to carry. */

// True iff the calling task IS bx_flash_worker already -- see
// adaptive_tune.c's adaptive_tune_clear_ki_baseline() for why this matters:
// autotune_engine_accept() can reach that function while already running on
// this worker (UART bridge accept path), and dispatching a second job onto
// a busy, non-recursive-mutex-guarded, depth-1-queue worker from inside the
// first one deadlocks the board permanently. Declared by hand for the same
// reason the function above is.
bool uart_bridge_ext_is_on_flash_worker(void);

extern const char *ADAPTIVE_TUNE_TAG;

// ---------------------------------------------------------------------
// Guards -- every numeric bound this module can refuse an update on.
// Shared across all three split files; see adaptive_tune.c's original
// top-of-file comment block (preserved there) for the reasoning behind each
// one -- moved here unchanged, not renumbered, so that history stays
// findable by constant name.
// ---------------------------------------------------------------------

#define ADAPTIVE_TUNE_SETTLE_MIN_S 180.0f
#define ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S 0.003f

// 2026-09-01, defect proven against REAL captured hardware data (coupid6:
// six 10-minute dwells/zone, 30/38/46/54/62/70 C, all three zones): the
// slope test above watches ACTUAL_C only. On an under-damped zone (this
// plant, at these gains) temperature can pass through a coincidental flat
// spot mid-oscillation while DUTY is still swinging hard -- zone 0's duty
// inside the 46 C dwell went 0.023 -> 0.19 -> 0.144, a ~0.167 abs / ~88%-of-
// value swing, well after the slope test would have already fired on a
// flat moment. A dwell IS the DC-gain measurement (K = rise_c/duty at
// steady state -- profile_executor.c zeroes target_rate_c_per_s in a
// dwell, so duty there is pure hold term, no feedforward to explain the
// motion away), so committing a still-moving duty as u_steady biases K
// directly, and every downstream fit (diagonal refine, coupled solve) that
// consumes it inherits the bias silently.
//
// tools/PcTools/src/kilnctrl/coupled_ident.py's settle_criterion_audit()
// mirrors this firmware's settle test and then checks whether duty kept
// moving AFTER the settle instant, over the REST of the same dwell window
// -- a diagnostic this firmware cannot run in real time (it does not know
// the future). The realtime-causal equivalent this module CAN run is to
// track duty's own range over the settle window it has already seen (from
// this dwell's first valid tick through the tick the slope test fires):
// on a genuinely oscillating plant, duty is already ranging throughout
// that window, not only afterward, so this check catches the same failure
// mode the offline audit does, just looking backward instead of forward.
//
// Thresholds mirror that audit's own UNSTABLE_DUTY_RANGE_ABS/FRAC exactly
// (not independently re-derived) so a reading this firmware accepts and one
// the audit does not flag are the same reading:
//   - ABS (0.05): this file's own ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION
//     (0.03) is already this module's floor for "enough duty energy in an
//     observation to trust the ratio at all" -- a swing bigger than that
//     floor is not a small ripple riding on a steady value, it is large
//     enough to itself move the reading across the boundary that decides
//     whether an observation is trustworthy in the first place.
//   - FRAC (25%): the measured defect itself was a ~23%-of-value swing and
//     MUST be rejected -- 25% is the smallest round number that still
//     clears it, not a number chosen to make a test fixture pass.
#define ADAPTIVE_TUNE_DUTY_STABILITY_ABS 0.05f
#define ADAPTIVE_TUNE_DUTY_STABILITY_FRAC 0.25f

#define ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION 0.03f

#define ADAPTIVE_TUNE_RING_CAPACITY 12

#define ADAPTIVE_TUNE_MIN_OBSERVATIONS 4u

#define ADAPTIVE_TUNE_MIN_DUTY_SPREAD 0.05f

#define ADAPTIVE_TUNE_MAX_JUMP_RATIO 5.0f

#define ADAPTIVE_TUNE_BLEND_ALPHA 0.15f
#define ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE 0.20f

// docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md: an ABSOLUTE
// ceiling on a blended/bootstrapped K_dc, checked in adaptive_tune_refine_
// zone_locked() (adaptive_tune_model.c) in addition to (never instead of)
// the ratio/fractional-move guards above. Those guards alone are NOT a
// bound: each one looks fine per-run, but composed across many accepted
// runs they let K_dc walk arbitrarily far, because -- before this pass --
// their own reference point was the live, already-adapted model_k_dc, which
// they themselves had just moved. Literally ZONE_AUTOTUNE_K_DC_MAX
// (zones_config_accessors.h), not a second, independently-chosen number --
// see that constant's own comment for the physical derivation (this
// system's own existing answer to "what temperature rise could this kiln
// ever physically/safely report", ZONE_MAX_TEMP_C_MAX) so the two constants
// can never silently drift apart.
#define ADAPTIVE_TUNE_K_DC_ABS_MAX ZONE_AUTOTUNE_K_DC_MAX

#define ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC 0.005f

#define ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION 0.05f

#include "nvs_key_check.h" /* NVS_KEY_LEN_CHECK -- see that header */

#define ADAPTIVE_TUNE_NVS_PARTITION "kiln_nvs"
#define ADAPTIVE_TUNE_NVS_NAMESPACE "adap_tune"
NVS_KEY_LEN_CHECK(ADAPTIVE_TUNE_NVS_PARTITION);
NVS_KEY_LEN_CHECK(ADAPTIVE_TUNE_NVS_NAMESPACE);
// PID_EXPANSION_PLAN.md 3.3 "consolidate the opt-in flag": en_mask is no
// longer this module's own source of truth for the opt-in (that moved to
// zone_cfg_t::adaptive_tune_enabled, zones_config_get/set_adaptive_tune_
// enabled() -- see zones_http.h's own comment) -- both keys stay defined
// here ONLY because adaptive_tune_migrate_enable_flags() (adaptive_tune.c)
// still needs to read en_mask once, at boot, to carry an upgrading board's
// prior choice into its new home, and to write en_migrated so it never
// consults en_mask again after that.
#define ADAPTIVE_TUNE_NVS_KEY_ENMASK "en_mask"
#define ADAPTIVE_TUNE_NVS_KEY_ENMASK_MIGRATED "en_migrated"
NVS_KEY_LEN_CHECK(ADAPTIVE_TUNE_NVS_KEY_ENMASK);
NVS_KEY_LEN_CHECK(ADAPTIVE_TUNE_NVS_KEY_ENMASK_MIGRATED);

// P1/K6: the Ki-diagnosis baseline (adaptive_tune_zone_t.ki_baseline/
// ki_baseline_valid) persisted alongside en_mask above -- see adaptive_
// tune.c's save_kibase_job()/adaptive_tune_init() for the read/write sides
// and this header's adaptive_tune_kibase_blob_t for the on-disk shape. ONE
// blob (not a second u8 key) so the two guard values for a given zone can
// never desync in storage -- see adaptive_tune_kibase_blob_t's own comment.
#define ADAPTIVE_TUNE_NVS_KEY_KIBASE "ki_base"
NVS_KEY_LEN_CHECK(ADAPTIVE_TUNE_NVS_KEY_KIBASE);

// docs/FILESYSTEM_USER_DATA_PLAN.md section 5, item 9 (adaptive-tune state):
// this task's own audit (ada18d3262, "adaptive-tune state was judged
// re-derivable over one firing") licenses a SIMPLER treatment than zones
// config/relay cycles get -- no per-zone divergence forensics, just the same
// generic pref_cfg_fs.h bridge every other small fixed-size struct in this
// codebase uses (unit_pref/ramp_assist/display_power/relay_names), applied
// to the WHOLE adaptive_tune_kibase_blob_t as one document. Losing this file
// (partition wipe, corrupt file) costs at most one firing's worth of
// baseline re-latching, not a safety-relevant fact -- see adaptive_tune.c's
// kibase_file_validate() for the one structural check applied (the mask
// cannot reference a zone index that does not exist), everything else is
// trusted the same way the NVS blob always was.
#define ADAPTIVE_TUNE_KIBASE_FILE_PATH "ki_base.dat"
#define ADAPTIVE_TUNE_NVS_KEY_KIBASE_REV "kibase_rev"
NVS_KEY_LEN_CHECK(ADAPTIVE_TUNE_NVS_KEY_KIBASE_REV);

#define ADAPTIVE_TUNE_FIT_MIN_DENOM 1e-4

// ---------------------------------------------------------------------
// Full coupled identification (Layer 2) -- guards.
// ---------------------------------------------------------------------

#define ADAPTIVE_TUNE_JOINT_RING_CAPACITY 24

#define ADAPTIVE_TUNE_COUPLED_OBS_MARGIN 2u

#define ADAPTIVE_TUNE_JOINT_MIN_DUTY ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION

#define ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE 6.0f

#define ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA ADAPTIVE_TUNE_BLEND_ALPHA

#define ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS 50.0f
#define ADAPTIVE_TUNE_COUPLING_PRIOR_NEAR_ZERO 1e-3f

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis from dwells -- guards.
// ---------------------------------------------------------------------

#define ADAPTIVE_TUNE_KI_TRACE_CAPACITY 24
#define ADAPTIVE_TUNE_KI_MIN_SAMPLES 12

#define ADAPTIVE_TUNE_KI_NOISE_FLOOR_C 0.05f

#define ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C 0.3f

#define ADAPTIVE_TUNE_KI_OFFSET_MAX_OVER_MEAN 1.6f

#define ADAPTIVE_TUNE_KI_MIN_CROSSINGS 4u

#define ADAPTIVE_TUNE_KI_CYCLE_REGULARITY_MAX 0.5f

#define ADAPTIVE_TUNE_KI_FLOOR_DUTY_VARIANCE 5e-4f
#define ADAPTIVE_TUNE_KI_FLOOR_DUTY_RAIL_BAND 0.05f

#define ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE 0.20f

// K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md): this bound is no
// longer READ by any production code -- adaptive_tune_ki.c stopped writing
// gains entirely (SIMC, adaptive_tune_model.c, is now the sole writer, and
// its own plausibility envelope is autotune_baseline_k_dc/ADAPTIVE_TUNE_MAX_
// JUMP_RATIO, a separate mechanism this constant never fed). Left defined,
// not deleted: it is exactly the "5x an autotuned baseline is implausible"
// reasoning below, independent of which layer would apply it, and the
// audit's own section 4.1 recommends rebuilding a Ki corrector later on
// ku_estimate/tu_estimate_s rather than deleting the file -- a rebuilt
// corrector would want a plausibility bound of this same shape.
//
// P2/K6 (SUPERSEDES K5, historical -- see K9 above for current status): K5
// originally set this to 50.0f, justified against
// this file's OWN synthetic convergence test needing ~27x
// (test_ki_diagnosis_converges_under_closed_loop_plant_feedback(), driven by
// that test's arbitrary KI_TEST_ERR_K constant). An opus review correctly
// flagged that as circular -- the plant model is a free parameter of the
// test fixture, not a real constraint, and mutating KI_TEST_ERR_K (8.0 ->
// 2.0, convergence ~6.7x instead of ~27x) left every test green, proving
// the fixture placed no actual bound on what "50x" should be.
//
// Re-derived against plant reasoning instead: this layer only ever runs
// AFTER a zone already has a SIMC-derived Ki from an actual FOPDT model (see
// adaptive_tune_refine_ki_locked()'s "no existing positive Ki to refine" guard). If
// that model is a reasonably good description of the zone, the Ki diagnosis
// should need only a modest correction to remove a genuine steady-state
// offset or damp a genuine oscillation -- not many multiples of the
// autotuned value. This file already has a considered opinion on "how far
// is a fit allowed to move from a trusted prior before it stops being a
// refinement and starts being evidence the MODEL itself is wrong": ADAPTIVE_
// TUNE_MAX_JUMP_RATIO (5.0x), applied to the diagonal K_dc fit for exactly
// this reason (see its own comment: "a genuinely aged element drifts run
// over run, it does not 5x between one firing and the next"). Reusing that
// same 5x figure here keeps both layers' plausibility bounds internally
// consistent: a zone whose closed-loop Ki need would exceed 5x its own
// autotuned baseline is, by this file's own standard, not "a bit off" -- it
// is telling us the K_dc/tau/dead_time model that Ki was derived from is
// wrong, and the correct response is a fresh autotune (a new FOPDT
// identification), not continued blind integral growth chasing a symptom of
// the wrong model. adaptive_tune_refine_ki_locked()'s cumulative-bound refusal message
// says exactly this when it binds.
//
// Still an order of magnitude above where a single per-run move
// (ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE, 20%) could land by accident, and
// nowhere near the 850.6x runaway K5 was written to stop -- see test_ki_
// diagnosis_runaway_under_constant_error_is_capped_by_cumulative_bound() for
// the negative-input case this must still cap, and test_ki_diagnosis_
// converges_under_closed_loop_plant_feedback() for the positive case (a
// fixture re-derived to converge well inside 5x, not defined BY 5x -- see
// that test's own comment on why KI_TEST_ERR_K's value no longer determines
// what the guard permits).
#define ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT 5.0f

#define ADAPTIVE_TUNE_KI_RELAY_HYSTERESIS_C 0.0f

// ---------------------------------------------------------------------
// Shared state.
// ---------------------------------------------------------------------

typedef struct {
    float duty;
    float rise_c; // actual_c - ambient_c at the settled instant
} adaptive_tune_obs_t;

typedef struct {
    bool enabled;

    bool  dwelling_prev;
    bool  settle_start_valid;
    float settle_start_c;
    float settle_elapsed_s;
    bool  recorded_this_dwell;

    // Duty range tracked over the SAME settle window as settle_start_c
    // above (reset alongside it) -- see ADAPTIVE_TUNE_DUTY_STABILITY_ABS/
    // FRAC's own comment for why this exists and what it catches.
    float settle_duty_min;
    float settle_duty_max;

    adaptive_tune_obs_t ring[ADAPTIVE_TUNE_RING_CAPACITY];
    uint32_t ring_count;
    uint32_t ring_head; // index of the OLDEST entry
    uint32_t observations_lifetime;

    bool     has_applied;
    float    prior_k_dc;
    float    applied_k_dc;
    float    last_delta_pct;
    uint8_t  last_applied_profile_id;
    uint32_t last_applied_unix_s;
    char     last_refusal_reason[96];

    float    trace_t_s[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    float    trace_actual_c[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    float    trace_duty[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    uint32_t trace_count;
    uint32_t trace_head;
    float    trace_elapsed_s;

    // K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md): the K8 fuzzy-
    // state snapshot fields that used to live here (trace_fuzzy_snapshot_
    // valid/trace_fuzzy_accessor_failed/trace_fuzzy_active/trace_fuzzy_pct)
    // are removed -- adaptive_tune_ki.c no longer writes gains, so there is
    // nothing left for that guard to protect. See adaptive_tune_ki.c's own
    // top comment.

    uint32_t joint_observations;
    bool     coupled_attempted;
    bool     coupled_applied;
    uint8_t  coupled_cells_changed;
    char     coupled_refusal_reason[96];
    uint8_t  ki_verdict;
    float    ki_correction_pct;
    bool     ki_applied;
    char     ki_refusal_reason[96];

    // P1/K5: this zone's autotuned baseline Ki, latched once (see ADAPTIVE_
    // TUNE_KI_CUMULATIVE_MAX_MULT) the first time adaptive_tune_refine_ki_locked()
    // reaches a live Ki, never overwritten after. Persisted to NVS alongside
    // en_mask (adaptive_tune_kibase_blob_t below) -- see adaptive_tune.c's
    // save_kibase_job()/adaptive_tune_init() -- specifically so a power
    // cycle cannot re-latch this from an already-grown live Ki. Before that
    // fix this field lived ONLY here, in RAM: measured, one reboot re-latched
    // it from the just-grown Ki and reached the exact 850.6x runaway the
    // cumulative bound exists to stop (see P1's own review finding).
    bool     ki_baseline_valid;
    float    ki_baseline;

    // ---------------------------------------------------------------------
    // U1: one-click revert (PID_EXPANSION_PLAN.md 3.3) -- a snapshot of
    // exactly what this zone's Kp/Ki/Kd, K_dc/tau/dead_time and Ki-diagnosis
    // baseline were IMMEDIATELY BEFORE the last change THIS MODULE applied
    // (either adaptive_tune_refine_zone_locked()'s SIMC recompute or
    // adaptive_tune_refine_ki_locked()'s Ki-only correction -- whichever ran
    // most recently; the two never both apply in the same run, see
    // adaptive_tune_run_end()'s D5 comment). RAM-only, per-BOOT, same
    // lifetime as has_applied above (its own comment explains why: the
    // change being described is itself only tracked for this boot) --
    // deliberately NOT persisted to NVS, so a revert is only ever offered
    // for a change this same boot session made. adaptive_tune_capture_
    // revert_locked() (adaptive_tune.c) is the ONE place that writes these,
    // called by both split files right before their respective commits;
    // adaptive_tune_revert() (adaptive_tune.c) is the only reader, and
    // clears revert_available once consumed (a revert is one-shot).
    bool     revert_available;
    float    revert_kp, revert_ki, revert_kd;
    float    revert_k_dc, revert_tau_s, revert_dead_time_s;
    bool     revert_ki_baseline_valid;
    float    revert_ki_baseline;
} adaptive_tune_zone_t;

extern adaptive_tune_zone_t adaptive_tune_zones[MAX31856_CHANNEL_COUNT];

typedef struct {
    float duty[MAX31856_CHANNEL_COUNT];
    float rise_c[MAX31856_CHANNEL_COUNT];
} adaptive_tune_joint_obs_t;

extern adaptive_tune_joint_obs_t adaptive_tune_joint_ring[ADAPTIVE_TUNE_JOINT_RING_CAPACITY];
extern uint32_t adaptive_tune_joint_ring_count;
extern uint32_t adaptive_tune_joint_ring_head;
extern uint32_t adaptive_tune_joint_observations_lifetime;
extern bool adaptive_tune_joint_dwell_row_committed;
extern float adaptive_tune_joint_last_duty[MAX31856_CHANNEL_COUNT];
extern float adaptive_tune_joint_last_rise_c[MAX31856_CHANNEL_COUNT];
extern bool  adaptive_tune_joint_last_valid[MAX31856_CHANNEL_COUNT];

extern SemaphoreHandle_t adaptive_tune_lock; // guards adaptive_tune_zones/adaptive_tune_joint_* above; taken only from this module's own
                                  // three files, never across profile_executor.c's s_exec.lock (see
                                  // adaptive_tune.h's doc comment on the lock order this keeps)
extern bool adaptive_tune_lock_ready;

void adaptive_tune_ensure_lock(void);

// P1: on-disk shape for the persisted Ki baseline -- ONE blob (not a mask
// byte plus a separate values blob) so a torn/partial write can never leave
// the valid bitmask and the values array disagreeing about which zones have
// a real baseline; a single nvs_set_blob() call is already atomic from this
// module's point of view (NVS itself guarantees a key's write is all-or-
// nothing). `mask` bit zi set means adaptive_tune_zones[zi].ki_baseline_valid should
// load true with vals[zi] as its baseline.
typedef struct {
    uint8_t mask;
    float   vals[MAX31856_CHANNEL_COUNT];
} adaptive_tune_kibase_blob_t;

// adaptive_tune_set_refusal()/adaptive_tune_set_reason(): small vsnprintf-into-field helpers shared by
// all three split files (adaptive_tune_refine_zone_locked(), adaptive_tune_refine_coupled_
// locked(), adaptive_tune_refine_ki_locked() each report their own guard refusals
// through one of these). adaptive_tune_set_refusal() always writes z->last_refusal_reason
// specifically; adaptive_tune_set_reason() takes an explicit destination buffer for the
// coupled/ki reason fields.
void adaptive_tune_set_refusal(adaptive_tune_zone_t *z, const char *fmt, ...);
void adaptive_tune_set_reason(char *buf, size_t bufsz, const char *fmt, ...);

// Cross-file locked helpers -- each defined in its own split file, called
// only from adaptive_tune_run_end() (adaptive_tune.c), always with adaptive_tune_lock
// already held.
bool adaptive_tune_refine_zone_locked(uint8_t zi, uint8_t profile_id);
void adaptive_tune_refine_coupled_locked(uint8_t zi);
void adaptive_tune_refine_ki_locked(uint8_t zi, const profile_exec_firing_stats_t *stats);

// U1: snapshots z's revert_* fields from the given PRE-CHANGE values --
// called by adaptive_tune_refine_zone_locked() (adaptive_tune_model.c) and
// adaptive_tune_refine_ki_locked() (adaptive_tune_ki.c) each right before
// their own zones_config_set_model()/set_pid() commit, always with
// adaptive_tune_lock already held (same convention as every other locked
// helper here). Defined in adaptive_tune.c, the one file that also defines
// adaptive_tune_revert() (the only reader of these fields).
void adaptive_tune_capture_revert_locked(adaptive_tune_zone_t *z, float kp, float ki, float kd, float k_dc,
                                          float tau_s, float dead_time_s);

#endif // ADAPTIVE_TUNE_INTERNAL_H
