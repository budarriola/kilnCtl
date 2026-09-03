// zones_config_json -- HTTP-free core of zones_http.c's per-zone/timing-
// profile configuration: the on-flash struct layouts (current and every
// historical version), the pure validation/clamping/conversion logic, and
// the application/x-www-form-urlencoded field parsers zones_post_handler()
// drives. Split out of zones_http.c following dashboard_json.c's precedent
// (see that file's own header comment): none of these functions ever touch
// httpd_req_t or esp_http_server.h, so they are host-testable without
// pulling in the HTTP server stack, and this file stays the single source of
// truth for "what is a valid zones_cfg_t" whether the caller is a live POST,
// an NVS load, or a kiln_cfg_store backup restore.
//
// zone_cfg_t/zone_timing_profile_t/zones_cfg_t (the CURRENT on-flash/in-RAM
// layout) moved here too, not just the functions that operate on them --
// zones_http.c's own s_zones.cfg still needs the type, and there is no
// meaningful way to split "the logic that validates/converts a zones_cfg_t"
// from "the definition of zones_cfg_t" without one half or the other reaching
// across a header for a type it cannot otherwise name. They were previously
// private to zones_http.c (never exposed via zones_http.h, which only ever
// dealt in scalars through its zones_config_get_*()/set_*() accessors); this
// split is what makes exposing them here necessary, not a change in what
// uses them -- every existing reader/writer of these fields is still exactly
// the same two .c files, now sharing one header instead of one file owning
// both.
//
// The historical zone_cfg_v1_t..v10_t/zones_cfg_v1_t..v10_t on-flash
// snapshots below are declared here too, not kept private to
// zones_config_json.c -- test_zones_http.c #includes zones_http.c textually
// (to reach parse_zone_fields(), a `static` function with no other seam) and
// references several of these historical layouts directly to stage an old-
// version blob, so they must be visible from any translation unit that
// includes zones_http.c, not just from zones_config_json.c's own
// convert_zone_v*()/convert_versioned_blob_to_current().
//
// Deliberately NOT moved here: parse_zone_fields(). It reads the live
// s_zones.cfg.zones[] global to run a settings_source cycle check against
// the rest of the board's live config, exactly the kind of file-scope
// mutable state this split exists to keep out of this module -- see
// zones_http.c's own copy for why it stays there.
#ifndef ZONES_CONFIG_JSON_H
#define ZONES_CONFIG_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "MAX31856.h" /* MAX31856_CHANNEL_COUNT */
#include "uart_task_ids.h" /* THERMO_TC_T -- ZONE_TC_TYPE_MAX_REAL below */
#include "zones_http.h" /* ZONE_NAME_MAX_LEN, TIMING_PROFILE_NAME_MAX_LEN, and every
                         * per-field bound (ZONE_*_MAX/MIN) validate_zones_cfg() and
                         * the field parsers below check against */

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever zones_cfg_t's on-flash layout changes -- see
 * zones_config_json.c's decode_zones_blob()/convert_versioned_blob_to_current()
 * and zones_http.c's nvs_save()/nvs_load_from(), which stamp/check this same
 * value. Shared because both files must agree on what "the current version"
 * means: nvs_save() writes it, decode_zones_blob() decides whether a stored
 * blob needs migrating against it. */
#define ZONES_CFG_VERSION 15


/* MAX31856 CR1.TC[3:0] nibble values 0x00-0x07 name a real thermocouple type
 * (B/E/J/K/N/R/S/T); 0x08-0x0F are the part's voltage-input modes, not
 * thermocouples. This operator-facing config rejects anything past
 * THERMO_TC_T -- see zones_http.c's original comment on this macro (git
 * history) for the full rationale; the raw UART debug path
 * (MAX31856_configure()) deliberately accepts the wider 0-0x0F range and is
 * untouched by this bound. */
#define ZONE_TC_TYPE_MAX_REAL THERMO_TC_T

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c; /* applied via zones_config_apply_cal() by dashboard_http.c and
                         * profile_executor.c, but not by uart_bridge.c -- see header */
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr; /* user-entered ceiling; 0 = never configured */
    float sanity_rate_c_per_min; /* direction/rate sanity threshold; 0 = never
                                   * configured -- see zones_config_get_sanity_rate() */
    float max_temp_c;        /* guard 5; 0 = not set, no ceiling */
    float min_temp_c;        /* guard 5; page defaults this to -20 */
    /* TODO.md 6A.9's "per-relay window_ms/min_on_ms/min_off_ms becoming
     * page-configurable" -- stored per zone rather than per physical relay,
     * since that's the granularity profile_executor.c/autotune_engine.c
     * actually apply a heater_output_cfg_t at (one zone's relay group
     * switches together as a unit; there's no per-individual-relay timing
     * concept anywhere in the control path). Float, not uint32_t, matching
     * every other field's parse_float_field()/JSON round-trip in this file
     * -- milliseconds up to 600000 round-trips exactly through a float's
     * 24-bit mantissa, so there's no precision cost to staying consistent
     * with the rest of the struct. 0 = "not configured," same convention as
     * sanity_rate_c_per_min -- the caller substitutes
     * PROFILE_EXECUTOR_DEFAULT_*_MS. */
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    /* TODO.md 6A.3's remaining named guard thresholds, promoted from
     * firmware-wide constants to per-zone override (2026-08-16) -- see
     * thermal_guard.h's doc comment for the full "0 substitutes a firmware
     * default, does NOT disable the guard" convention these all share.
     * OPTIONAL on POST, same reason z%u_xzone below is (see
     * parse_zone_fields()): an operator who never touches these keeps the
     * firmware defaults, and older clients (pc_tools/MCP, the test
     * harnesses) must not start getting 400s for fields they've never heard
     * of. */
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    /* Guard 8, cross-zone plausibility (TODO.md 6A.3): degC this zone's raw
     * reading may differ from its worst-disagreeing peer's before the guard
     * trips. 0 = "not configured", which DISABLES the guard rather than
     * substituting a default -- the opposite convention to
     * sanity_rate_c_per_min above, and deliberately so: a plausible number
     * here depends on how strongly this particular kiln's zones couple
     * (TODO.md 6A.5's cross-gain matrix), and a hand-picked default would
     * either nuisance-trip a kiln that genuinely stratifies or be so wide it
     * catches nothing. Left blank until the operator has a measurement. */
    float cross_zone_max_delta_c;
    /* The FOPDT plant model autotune (TODO.md 6A.4) fitted for this zone,
     * kept so 6A.2's feedforward term can be computed from it:
     * u_ff = (T_sp - T_ambient)/K + (dT_sp/dt)*tau/K. Until now the fit was
     * used once to derive Kp/Ki/Kd and then thrown away with the run, which
     * meant the feedforward had nothing to stand on after a reboot -- and a
     * step test costs the operator hours of real kiln heat, so re-measuring
     * it on every boot is not an option.
     *
     * Stored here rather than in autotune_engine.c for the same reason the
     * gains are: 6A.4 makes zones_http the single owner of persisted zone
     * config, and autotune must not touch NVS itself.
     *
     * 0 in ANY of the three means "no model identified for this zone" and
     * callers must treat it as "cannot answer" (see the getter's header
     * comment) -- all three, because a physically meaningful model needs a
     * non-zero gain AND a non-zero time constant, and dividing by either
     * zero in the feedforward expression is exactly the failure this
     * convention exists to prevent. A genuinely zero dead time is not
     * physically reachable on a kiln (tens of seconds is typical), so
     * nothing real is lost by folding it into the same rule. */
    float model_k_dc;        /* static gain, degC per unit duty at steady state */
    float model_tau_s;       /* first-order time constant, seconds */
    float model_dead_time_s; /* transport delay L, seconds */
    /* ---- PID_EXPANSION_PLAN.md Phase 2 (v10, 2026-08-30). Appended at the
     * tail of the float group, same discipline as model_* above. ---- */
    float fuzzy_strength_pct;      /* section 3.3's "Adjustment strength", 0-100. 0 = no fuzzy
                                    * adjustment, i.e. behaves exactly like classic PID. */
    /* ---- ZONES_CFG_VERSION 10->11 (2026-08-30): widened from a single
     * (coeff, neighbor) pair to a full directed row. Bench-measured coupling
     * is BOTH asymmetric (1->0 measured 1.86x stronger than 0->1) AND
     * multi-neighbor (zone 1 has two very different cross-gains, one per
     * peer) -- a single pair can only ever hold one of a zone's N-1
     * neighbors and silently discards the rest. See ZONES_CFG_VERSION's
     * 10->11 comment for the full migration story.
     *
     * coupling_coeff[j] = THIS zone's measured steady-state response, in
     * degC per unit commanded duty (0..1) at ZONE j's heater -- the exact
     * same "raw FOPDT static gain" unit convention model_k_dc already uses
     * above, deliberately, so the feedforward term can combine them without
     * an extra unit conversion: u_ff contribution from neighbor j is
     * -coupling_coeff[j] * (T_j - T_j_setpoint) / coupling_coeff[own index]
     * (Phase 3b, a later pass; this file only stores the coefficients).
     * NOT a ratio to self-gain -- storing a raw gain, not a dimensionless
     * fraction, is what lets autotune_engine.c persist a fitted cross-gain
     * directly with no extra scaling step (see finalize_fit()'s comment).
     *
     * coupling_coeff[own index] (the diagonal) is UNUSED and MUST stay 0 --
     * a zone's response to its own heater is model_k_dc, already stored
     * separately; validate_zones_cfg() enforces the diagonal is exactly 0
     * rather than silently ignoring whatever a client sends there.
     *
     * 0 in any off-diagonal cell = "no coupling measured against that
     * neighbor" -- the safe default AND the degrade-to-today behavior,
     * unchanged from the single-pair layout this replaces.
     *
     * Sign: kept non-negative, same bound as the single-pair layout had
     * (ZONE_COUPLING_COEFF_MAX, unsigned). The feedforward's own minus sign
     * (-coupling_coeff[j] * (T_j - sp_j), see above) already gives the
     * needed direction -- a hotter-than-setpoint neighbor always pulls this
     * zone's feedforward output down, a colder one pushes it up -- so a
     * second, independent sign carried on the coefficient itself would be
     * redundant at best and a double-negative bug at worst. This was a
     * deliberate decision for this pass, not an oversight: nothing measured
     * on the bench so far (all three logged coefficients are positive
     * cross-heating gains) needs a negative coefficient to be represented,
     * and the ceiling here is a typo/garbage filter, not a physics bound. */
    float coupling_coeff[MAX31856_CHANNEL_COUNT];
    /* ---- ZONES_CFG_VERSION 11->12 (2026-08-31): the FOPDT tau/dead-time
     * autotune already fits for every off-diagonal cell of coupling_coeff[]
     * above (finalize_fit()'s per-peer pid_autotune_fit_fopdt() call,
     * autotune_engine.c) but, until now, threw away -- only the gain
     * (k_gain_c_per_duty) was persisted; tau/L died with the RAM-only
     * s_at.coupling the moment the run ended. DATA PLUMBING ONLY: no
     * consumer reads either array yet, same as coupling_coeff was itself
     * pure storage for one pass before profile_executor.c's Phase 3b wired
     * it into the feedforward.
     *
     * SAME orientation as coupling_coeff -- coupling_tau_s[j]/
     * coupling_dead_time_s[j] are THIS zone's (the affected zone's) fitted
     * response to a step at zone j's (the stepped zone's) heater, i.e.
     * zones[affected].coupling_tau_s[stepped]. The diagonal is unused and
     * MUST stay 0, identical to coupling_coeff's own diagonal rule.
     *
     * 0 in either array = "not measured" -- the same convention
     * coupling_coeff/model_tau_s/model_dead_time_s already use, and the only
     * value an older (pre-v12) blob's migrated zones can carry, since no
     * prior version stored these at all. Units: seconds, same as
     * model_tau_s/model_dead_time_s. Bounded by ZONE_MODEL_TIME_MAX_S, the
     * same ceiling the diagonal (self) tau/dead-time already use -- a
     * cross-zone thermal time constant has no reason to be a different order
     * of magnitude than a zone's own. */
    float coupling_tau_s[MAX31856_CHANNEL_COUNT];
    float coupling_dead_time_s[MAX31856_CHANNEL_COUNT];
    /* ---- Every remaining field is a uint8_t, deliberately grouped here at
     * the struct tail -- see this struct's own top-of-definition comment for
     * why (alignment padding, and the ZONES_CONFIG_BLOB_MAX_SIZE budget it
     * buys back). ---- */
    uint8_t relay_mask; /* bit N-1 = relay N belongs to this zone, N in 1..relay_count */
    uint8_t control_mode; /* zone_control_mode_t (TODO.md 6A.1) */
    /* 2026-08-21: the actual gap the owner reported -- "in the thermocouples
     * settings page i dont see anywhere i can select my thermocouple type."
     * MAX31856.c's tc_type field (CR1.TC[3:0]) was always THERMO_TC_K at
     * boot, hardcoded, never persisted, and never exposed here. This is
     * per-CHANNEL, not per-zone, even though it lives in the same
     * MAX31856_CHANNEL_COUNT-sized zones[] array indexed by i: slot i is
     * "channel i" for this field regardless of which zone(s) thermo_mask
     * says actually read that channel (a zone can combine several channels;
     * each physical channel still has exactly one real thermocouple wired to
     * it with exactly one type). Bounded to ZONE_TC_TYPE_MAX_REAL (0-7, the
     * eight real types) by both parse_zone_fields() and
     * migrate_zones_cfg_v1_to_current() -- the remaining CR1 nibble codes
     * are voltage-input modes, not thermocouples, and have no business being
     * reachable from this operator-facing page (see ZONE_TC_TYPE_MAX_REAL's
     * comment). Applied to hardware at boot by zones_http_start() below,
     * which is the fix for the other half of the bug: setting this over
     * UART today (thermo_owner_command_config_channel(), already wired) was
     * live only until the next reboot, because nothing read it back out of
     * NVS at bring-up. */
    uint8_t tc_type;
    /* TODO.md 10.8 (2026-08-17): which MAX31856 channels combine (mean of
     * valid readings, thermo_combine.c) into this zone's control
     * temperature -- bit N-1 = channel N, same convention as relay_mask
     * above. See zones_config_get_thermo_mask()'s doc comment (zones_http.h)
     * for the legacy-mapping default a 0 here falls back to when the field
     * was never explicitly supplied, which is what keeps this addition from
     * being the kind of silent-wipe field growth TODO.md 6A.1's
     * relay_cycles.c note (section 6A.1, 2026-08-12) warns against. */
    uint8_t thermo_mask;
    /* 2026-08-27: which of SaftyFW's ZONE_CT_CHANNEL_COUNT current-sense
     * channels feed this zone's live-current display -- bit N-1 = CT channel
     * N, same convention as relay_mask/thermo_mask above. See
     * ZONES_CFG_VERSION's 5->6 comment: purely a display mapping, no
     * exclusivity, may legitimately overlap another zone's ct_mask (one
     * physical CT probe clamped around a shared supply line feeding more
     * than one zone's element). 0 = no probe mapped to this zone yet. */
    uint8_t ct_mask;
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9), owner-report follow-up ("make it so
     * that i can have diffrent Safety timings and assign the zones to them ...
     * that way i dont need to copy the data multipal times"): replaces the
     * nine per-zone override floats v8 added directly on this struct
     * (guard_progress_duty_min .. ramp_lock_band_c -- see ZONES_CFG_VERSION's
     * 8->9 comment for the full migration story). Those nine numbers now live
     * once per zone_timing_profile_t, not once per zone; this index picks
     * WHICH profile in zones_cfg_t::timing_profiles[] this zone uses. Must be
     * < zones_cfg_t::timing_profile_count -- validate_zones_cfg() enforces
     * that, and the POST /api/zones handler bounds a submitted z%u_timingprofile
     * against however many profiles the same submission defines. Profile 0
     * always exists (timing_profile_count is never 0 in a valid config), so a
     * freshly zero-initialized zone_cfg_t already points at a real profile --
     * the same "0 is always a safe, meaningful value" property control_mode/
     * thermo_mask/etc. already have, not a dangling reference. */
    uint8_t timing_profile;
    /* Section 3.5's UI-only provenance marker: ZONE_SETTINGS_SOURCE_CUSTOM
     * (0xFF) = "this zone's own settings", otherwise the index of the zone
     * this one's dropdown claims to copy. Stored ONLY so the settings page
     * re-opens showing the right dropdown state -- the resolved values are
     * written into each zone's own fields on save, so NOTHING in the control
     * loop ever reads this. Note 0 is a real value here ("copies zone 0"),
     * not an empty default: see convert_zone_v9(). */
    uint8_t settings_source;
    /* ---- ZONES_CFG_VERSION 12->13 (2026-09-01, owner: "in the pid
     * stistics consider, maybe there should be 2 sets, one for the pid
     * tuneing that stays unless retuned, and another for the last fireing
     * run"). This is set 1 -- the TUNING-quality record, one snapshot per
     * zone that survives until the zone is next re-tuned (set 2, the
     * per-run FIRING quality history -- mean error/overshoot/IAE/ramp-dwell
     * split -- already existed before this pass and lives in
     * profile_executor.c, not here; the two are deliberately not merged,
     * see zones_page.html's "Tuning quality" vs "Firing quality" headings).
     *
     * Every field the autotune engine's fit already computes but, until
     * this pass, discarded the instant model_k_dc/model_tau_s/
     * model_dead_time_s above were written -- fopdt_model_t's settled/
     * extrapolation_converged/tau_consistent_with_gain flags (pid_
     * autotune.h), the tuning rule and identification method actually used,
     * the baseline/step-ambient temperatures the fit was measured from, and
     * the raw-vs-extrapolated rise (rise_inf_c - raw_rise_c tells a reader
     * how much of model_k_dc was extrapolated rather than directly
     * measured -- a large correction is a weaker fit, same reasoning
     * pid_autotune.h's own "RESIDUAL ACCURACY GAP" comment documents for
     * why this matters to a caller judging fit confidence).
     *
     * tuning_valid is the master gate, same "0/false means cannot answer"
     * convention as model_k_dc/model_tau_s/model_dead_time_s use for "no
     * model": false means every other tuning_* field below must be treated
     * as UNKNOWN, not read as a real (if zero-valued) measurement -- this is
     * what lets a v12 blob migrate its new fields to a safe "unknown" rather
     * than a value indistinguishable from "SIMC rule, step method, perfectly
     * settled", which is what an all-zero read would otherwise imply.
     *
     * INVALIDATION (the reset-one-side risk this whole feature exists to
     * avoid -- see zones_config_set_pid()'s own comment): every path that
     * changes pid_kp/pid_ki/pid_kd goes through zones_config_set_pid(),
     * which sets tuning_valid = false unconditionally before saving,
     * regardless of caller (autotune accept, a manual POST /api/zones/pid
     * edit, adaptive_tune.c's blended re-tune, backup_http.c's restore, or
     * the LCD UI/uart_bridge_ext.c path) -- a stale quality record pinned to
     * hand-edited gains would be worse than none. autotune_engine.c's
     * finalize accept() path re-establishes a fresh record via
     * zones_config_set_tuning_quality() immediately afterward, once the new
     * gains AND model are both already persisted, so the invalidate-then-
     * repopulate ordering never leaves a valid-looking stale record visible
     * in between. */
    uint8_t  tuning_valid;                      /* 0/1 -- same "0 means cannot answer" convention as
                                                  * every other bool-shaped uint8_t in this struct
                                                  * (relay_mask siblings), never a real C99 bool: this
                                                  * struct is a raw NVS blob, not a language-portable type */
    uint8_t  tuning_method;                     /* autotune_method_t raw value: 0=STEP, 1=RELAY */
    uint8_t  tuning_rule;                       /* autotune_rule_t raw value: SIMC/ZN/Tyreus-Luyben/Cohen-Coon */
    uint8_t  tuning_settled;                    /* fopdt_model_t::settled, 0/1 */
    uint8_t  tuning_extrapolation_converged;    /* fopdt_model_t::extrapolation_converged, 0/1 */
    uint8_t  tuning_tau_consistent;             /* fopdt_model_t::tau_consistent_with_gain, 0/1 */
    float    tuning_baseline_c;                 /* fopdt_model_t::baseline_c */
    float    tuning_step_ambient_c;             /* autotune_engine_status_t::step_ambient_c at fit time */
    float    tuning_raw_rise_c;                 /* fopdt_model_t::raw_rise_c -- measured, unextrapolated rise */
    float    tuning_rise_inf_c;                 /* fopdt_model_t::rise_inf_c -- the extrapolated asymptote
                                                  * actually used for model_k_dc; (rise_inf_c - raw_rise_c) is
                                                  * the extrapolation correction, i.e. how much of the gain
                                                  * was extrapolated rather than directly measured */
    uint32_t tuning_seq;                        /* monotonic per-zone counter, bumped by
                                                  * zones_config_set_tuning_quality() itself -- a cheap run
                                                  * identifier/ordering marker in place of a wall-clock
                                                  * timestamp this board has no guaranteed RTC for */
    /* ---- ZONES_CFG_VERSION 13->14 (2026-09-01, PID_EXPANSION_PLAN.md 3.3:
     * "consolidate the opt-in flag into the zone config blob"). adaptive_
     * tune.c's per-zone continuous-tuning opt-in used to live entirely in
     * its OWN NVS namespace ('adap_tune', key 'en_mask') -- purely because
     * this file was held live by another agent at the time that module was
     * written, not because the flag belongs there (see adaptive_tune.h's
     * former top comment, now updated). It belongs here, alongside every
     * other per-zone operator setting.
     *
     * Same "0/false means off" convention as control_mode/tc_type/etc
     * above: 0 = opted out (DEFAULT OFF, unchanged from the old home), 1 =
     * opted in. A blob migrated up from a pre-14 version gets 0 here from
     * convert_zone_v13()'s memset (this field did not exist before v14) --
     * that is NOT the real migration path for an upgrading board's actual
     * choice, though: the operator's old en_mask bit is carried forward
     * separately, once, by adaptive_tune_migrate_enable_flags()
     * (adaptive_tune.c), called at boot AFTER zones config has loaded. It
     * does not belong in convert_zone_v13() itself -- this file's version-
     * conversion helpers are deliberately pure, NVS-free struct math
     * (host-testable with no NVS at all), and the old value lives in a
     * completely different NVS namespace this file has no reason to know
     * about. */
    uint8_t  adaptive_tune_enabled;
    /* ---- ZONES_CFG_VERSION 14->15 (2026-09-02, PID_EXPANSION_PLAN.md 3.2
     * follow-up: "the matrix's OWN diagonal is better supported by the data
     * than substituting ff_k_dc"). STORAGE ONLY -- zone_coupling_solve.c
     * still substitutes ff_k_dc for G[row][row] and is UNCHANGED by this
     * pass; this field only makes the alternative persistable so a later,
     * separately reviewed pass can switch the solver behind a flag.
     *
     * coupling_diag_k_dc is THIS zone's own steady-state DC gain (degC per
     * unit commanded duty, exactly model_k_dc's/coupling_coeff[]'s unit
     * convention) as fitted by the SAME three-run settled-excitation
     * identification that produced this zone's row of coupling_coeff[] --
     * i.e. it is the diagonal cell of that identification's matrix, the one
     * cell coupling_coeff[own index] is contractually forbidden to hold (see
     * that field's own "MUST stay 0" comment). It is measured from the SAME
     * data as the off-diagonal cross-gains, at the same time, by the same
     * fit.
     *
     * Deliberately NOT named anything with "k_dc" alone, and deliberately
     * not folded into model_k_dc or ff_k_dc -- both of those are a DIFFERENT
     * identification (a single-zone step/relay test with every other zone
     * held at rest) and can legitimately disagree with this field on a real
     * board (measured condition number 4.64 vs 5.51, feasibility 65 degC vs
     * 60 degC, favoring THIS field over ff_k_dc in the analysis that
     * motivated adding it -- see PID_EXPANSION_PLAN.md section 3.2). A
     * caller that confused the two would silently swap one plant model for
     * another that happens to share units; see this repo's "split-module
     * missing name" bug class for why that ambiguity is worth a longer name.
     *
     * 0 = "not measured by the coupling identification" -- the same
     * convention coupling_coeff[]/coupling_tau_s[]/coupling_dead_time_s[]
     * already use, and the only value an older (pre-v15) blob's migrated
     * zones can carry, since no prior version stored this at all. Bounded by
     * ZONE_MODEL_K_MAX, the same ceiling model_k_dc already uses -- this is
     * the same physical quantity (a zone's own DC gain) from a different
     * fit, so it has no reason to need a different order-of-magnitude
     * ceiling. */
    float coupling_diag_k_dc;
} zone_cfg_t;


/* A named, reusable bundle of the nine thermal-timing numbers that used to be
 * typed once per zone (see zone_cfg_t::timing_profile's comment and
 * ZONES_CFG_VERSION's 8->9 comment for the full rationale). Every field here
 * keeps the identical "0 = not configured, the consuming module substitutes
 * its own named firmware default; 0 never disables" convention and the exact
 * same bounds these nine had as zone_cfg_t fields -- only WHERE they are
 * stored changed, not their meaning or validation. */
typedef struct {
    char name[TIMING_PROFILE_NAME_MAX_LEN + 1]; /* operator-entered label, e.g. "Default", "Fast bisque" */
    float guard_progress_duty_min;    /* guard 1 arms above this commanded duty */
    float guard_progress_window_s;    /* guard 1's no-progress window while heating */
    float guard_drift_hysteresis_c;   /* guard 4's settle band / drift threshold */
    float guard_frozen_eps_c;         /* guard 7: reading moves by less than this = frozen */
    float guard_cross_zone_period_s;  /* guard 8's sustained-disagreement window */
    float bangbang_hysteresis_c;      /* BANGBANG mode's switching band */
    float cooling_limited_margin_c;   /* "cannot cool fast enough" detection margin */
    float cooling_limited_hold_s;     /* ...sustained for this long before it counts */
    float ramp_lock_band_c;           /* ramp-rate lock engages within this of setpoint */
} zone_timing_profile_t;

typedef struct {
    uint8_t version; /* ZONES_CFG_VERSION at save time -- see nvs_load_from() */
    uint8_t thermo_count;
    uint8_t relay_count;
    /* TODO.md 6A.5 load-staggering: 0 = unlimited (default, existing
     * behavior unchanged). Global, not per-zone -- a breaker/supply limit
     * applies to the whole board, not one zone. Enforced by
     * profile_executor.c, not here; this struct only stores it. */
    uint8_t max_simultaneous_relays;
    /* TODO.md 6A.3's "default policy on a single-zone trip: abort the whole
     * firing" -- 0 (the zero-initialized default, matching a migrated v1
     * blob that predates this field) is that default; 1 is the explicitly
     * opted-in "continue with the other healthy zones" alternative the
     * bullet says needs justifying, not the abort. Global, same reasoning as
     * max_simultaneous_relays: a multi-zone firing's abort policy is a
     * whole-board decision, not a per-zone one. Enforced by
     * profile_executor.c's escalate_guard_trip(). */
    uint8_t continue_on_zone_trip;
    /* 2026-08-21, TODO.md owner-report task item 3: the RP2040 safety
     * processor has its OWN, physically independent MAX31856 thermocouple --
     * not one of the main board's MAX31856_CHANNEL_COUNT channels above, a
     * fourth (well, first) part on entirely separate hardware -- and its
     * type is set over the link by SAFETY_CMD_SET_CONFIG
     * (safety_link_send_set_config()). This is a SEPARATE setting from any
     * zone's tc_type above, deliberately: the safety processor's sensor is
     * wired to whatever thermocouple the operator physically attached to
     * ITS input, which has no reason to match any particular main-board
     * zone's channel (that is the whole point of it being an *independent*
     * cross-check -- see docs/HARDWARE.md and SaftyFW's own thermocouple
     * wiring). Defaults to THERMO_TC_K, matching what MAX31856.c has always
     * hardcoded for the main board's own channels, so a board that has never
     * touched this setting keeps behaving exactly as it does today. Mirrored
     * to the Pico by safety_link.c's poll task, not sent directly from this
     * file -- see that file's safety_sync_tc_type() for why (this module has
     * no reference to the SafetyLinkClass instance; main.c, which does, is
     * off-limits this pass) and for the "link was down when this changed"
     * re-apply-on-reconnect handling. */
    uint8_t safety_tc_type;
    zone_cfg_t zones[MAX31856_CHANNEL_COUNT];
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9): how many of timing_profiles[]
     * below's MAX31856_CHANNEL_COUNT slots actually hold a profile. Sized to
     * the zone count, not some larger operator-facing limit, because the
     * worst case that must always fit is "every zone wants its own distinct
     * profile" -- never more than one profile per zone could ever be useful.
     * Never 0 in a config validate_zones_cfg() has accepted: every
     * zone_cfg_t::timing_profile must resolve to a real slot, including a
     * freshly zero-initialized zone at profile index 0, so at least one
     * profile (index 0) must always exist. */
    uint8_t timing_profile_count;
    /* The named timing-profile bundles zones point at via
     * zone_cfg_t::timing_profile -- see that field's comment and
     * ZONES_CFG_VERSION's 8->9 comment for the full rationale. Only indices
     * [0, timing_profile_count) are meaningful; slots past that are unused
     * and not emitted by GET /api/zones. */
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    /* 2026-08-27 (ZONES_CFG_VERSION 7->8): how long the PC link may go silent
     * before a running firing is aborted. Global rather than per-zone: the
     * link is one wire to one PC, and its loss is a whole-board condition, not
     * something one zone experiences and another does not. 0 = not configured,
     * substituting PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS. Stored as float
     * for the same reason heater_window_ms is (see its comment) -- 30000ms
     * round-trips exactly, and every parse/emit helper in this file is
     * float-shaped. */
    float pc_link_abort_silence_ms;
    /* 2026-08-27 (ZONES_CFG_VERSION 6->7): CRC32 over this whole struct with
     * this field itself zeroed, stamped by nvs_save() (see compute_zones_crc())
     * and checked by decode_zones_blob() on every load of a CURRENT-version
     * blob. Appended at the true tail -- the one safe place to grow this
     * struct, unlike zone_cfg_t's own history of insertions mid-array-element
     * (see ZONES_CFG_VERSION's comment above). Older on-flash versions never
     * had this field; their integrity gate is the length-must-match-the-
     * claimed-version check plus validate_zones_cfg(), not a CRC. */
    uint32_t crc32;
} zones_cfg_t;

/* ---- Historical on-flash layouts (ZONES_CFG_VERSION 1..11) ---------------
 * EXACT field-for-field snapshots of what zone_cfg_t/zones_cfg_t looked like
 * at each prior version, recovered from this file's git history (see
 * zones_config_json.c's decode/convert functions for the full per-version
 * provenance notes) -- used ONLY to interpret a raw on-flash blob whose
 * length has already been checked against the size of EXACTLY the matching
 * struct before a single byte is copied out of it. None of these are ever
 * grown, edited, or reused for a different version. Declared here (not kept
 * private to zones_config_json.c) because test_zones_http.c #includes
 * zones_http.c textually and stages old-version blobs directly against
 * several of them. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
} zone_cfg_v1_t; /* v1 and v2 -- predates the 8 guard thresholds, thermo_mask, tc_type, ct_mask */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
} zone_cfg_v3_t; /* v3 -- adds the 8 guard thresholds; predates thermo_mask/tc_type/ct_mask */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask; /* appended at the tail -- still safe growth at this point */
} zone_cfg_v4_t; /* v4 -- adds thermo_mask; predates tc_type/ct_mask */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    uint8_t tc_type;   /* inserted HERE, before model_k_dc and thermo_mask -- NOT
                        * at the tail. This is the exact insertion that broke
                        * the old "grows at the tail only" migration for every
                        * zone after the first. */
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask;
} zone_cfg_v5_t; /* v5 -- adds tc_type (mid-struct); predates ct_mask */

/* v6/v7 shared the same zone layout, ending at ct_mask. v8 appends nine
 * per-zone fields after it, so that layout now needs its own snapshot -- this
 * is what zone_cfg_t looked like before this pass, field for field. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    uint8_t tc_type;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask;
    uint8_t ct_mask;
} zone_cfg_v7_t; /* v6/v7 -- predates the nine v8 per-zone override fields */

/* v8 appended the nine per-zone timing overrides directly to zone_cfg_v7_t's
 * layout; v9 (this pass) removes them again in favor of timing_profile, so
 * v8's zone layout now needs its own frozen snapshot too -- this is what
 * zone_cfg_t looked like for the one version that had these nine fields
 * living per-zone. NEVER edited, grown, or reused -- see this file's own
 * WARNING in ZONES_CFG_VERSION's 8->9 comment for why sizeof(this struct),
 * not sizeof(the current zone_cfg_t), is the only correct length for a v8
 * blob. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    uint8_t tc_type;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    float guard_progress_duty_min;
    float guard_progress_window_s;
    float guard_drift_hysteresis_c;
    float guard_frozen_eps_c;
    float guard_cross_zone_period_s;
    float bangbang_hysteresis_c;
    float cooling_limited_margin_c;
    float cooling_limited_hold_s;
    float ramp_lock_band_c;
} zone_cfg_v8_t; /* v8 -- the nine timing overrides lived here, per zone; predates timing_profile */

typedef struct {
    /* CORRECTED 2026-08-30. The first version of this frozen layout
     * interleaved the uint8_t members (relay_mask after name, control_mode
     * mid-floats, tc_type before the model floats) -- that is v8's shape,
     * not v9's. v9 deliberately groups every uint8_t at the tail, and the
     * live zone_cfg_t still carries the comment saying so.
     *
     * The consequence was not a subtle one: the interleaved form pads to a
     * different size (each isolated uint8_t rounds up to the next float's
     * alignment), so expected_len_for_version(9) could never match a real
     * stored blob, zones_config_json_decode_blob() would reject every commissioned
     * board's config as corrupt, and the board would boot to factory zone
     * defaults -- cal offsets, PID gains, guard thresholds, temperature
     * limits and relay/thermocouple wiring all silently gone on the first
     * boot after the update. The static asserts below exist so this cannot
     * regress unnoticed: a frozen historical layout that no longer matches
     * the bytes actually on flash is worse than no migration at all. */
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    /* ---- uint8_t tail, exactly as v9 grouped them ---- */
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
} zone_cfg_v9_t;

/* The whole point of a frozen historical layout is that it still describes
 * the bytes really sitting in flash on a v9 board. 116 = 16 (name) + 23*4
 * (floats) + 6 (the uint8_t tail) + 2 (tail padding to the struct's 4-byte
 * float alignment). If a future edit reorders or adds a member here, this
 * fires at compile time instead of wiping a commissioned kiln's config at
 * the next boot -- which is exactly what the interleaved first version of
 * this struct would have done. */
_Static_assert(sizeof(zone_cfg_v9_t) == 116,
               "zone_cfg_v9_t must match the on-flash v9 layout byte-for-byte (116 bytes)"); /* v9 -- predates fuzzy_strength_pct/coupling_coeff/coupling_neighbor_zone/settings_source */

/* Frozen v10 layout -- what zone_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 10->11), single coupling_coeff/coupling_neighbor_zone
 * pair and all. Same discipline as zone_cfg_v9_t just above: field order
 * hand-copied from v10's actual shape, never derived from the live struct,
 * so this keeps describing real on-flash bytes even after zone_cfg_t itself
 * changes shape again in some future pass. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    float fuzzy_strength_pct;
    float coupling_coeff;         /* v10's single scalar -- NOT the v11 row */
    float coupling_neighbor_zone; /* v10's single neighbor index */
    /* ---- uint8_t tail, exactly as v9/v10 grouped them ---- */
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
} zone_cfg_v10_t;

/* 128 = 16 (name) + 26*4 (floats: v9's 23 + fuzzy_strength_pct +
 * coupling_coeff + coupling_neighbor_zone) + 7 (the uint8_t tail: v9's 6 plus
 * settings_source) + 1 (tail padding to the struct's 4-byte float alignment).
 * Hand-computed, same as v9's own assert comment above requires -- never
 * sizeof(zone_cfg_t), which by the time this pass lands is already the v11
 * (row-coupling) shape, not v10's. */
_Static_assert(sizeof(zone_cfg_v10_t) == 128,
               "zone_cfg_v10_t must match the on-flash v10 layout byte-for-byte (128 bytes)"); /* v10 -- predates the coupling_coeff[] row */

/* Frozen v11 layout -- what zone_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 11->12), coupling_coeff[] row and all, predating
 * coupling_tau_s[]/coupling_dead_time_s[]. Same discipline as
 * zone_cfg_v10_t just above: field order hand-copied from v11's actual
 * shape, never derived from the live struct. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    float fuzzy_strength_pct;
    float coupling_coeff[MAX31856_CHANNEL_COUNT]; /* v11's row -- predates coupling_tau_s[]/
                                                    * coupling_dead_time_s[] */
    /* ---- uint8_t tail, exactly as v9/v10 grouped them ---- */
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
} zone_cfg_v11_t;

/* 132 = 16 (name) + 27*4 (floats: v10's 26 minus coupling_coeff/
 * coupling_neighbor_zone (2 scalars) plus the 3-wide coupling_coeff[] row,
 * i.e. 24 + 3 = 27) + 7 (the uint8_t tail, unchanged since v9) + 1 (tail
 * padding to the struct's 4-byte float alignment). Hand-computed AND
 * verified with ctypes against the live field layout before this assert was
 * written -- never sizeof(zone_cfg_t), which by the time this pass lands is
 * already the v12 (tau_s[]/dead_time_s[]) shape, not v11's. */
_Static_assert(sizeof(zone_cfg_v11_t) == 132,
               "zone_cfg_v11_t must match the on-flash v11 layout byte-for-byte (132 bytes)"); /* v11 -- predates coupling_tau_s[]/coupling_dead_time_s[] */

/* Frozen v12 layout -- what zone_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 12->13), coupling_tau_s[]/coupling_dead_time_s[]
 * and all, predating the tuning_* quality fields. Same discipline as
 * zone_cfg_v11_t just above: field order hand-copied from v12's actual
 * shape, never derived from the live struct. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    float fuzzy_strength_pct;
    float coupling_coeff[MAX31856_CHANNEL_COUNT];
    float coupling_tau_s[MAX31856_CHANNEL_COUNT];
    float coupling_dead_time_s[MAX31856_CHANNEL_COUNT];
    /* ---- uint8_t tail, exactly as v9/v10/v11 grouped them ---- */
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
} zone_cfg_v12_t;

/* 156 = 16 (name) + 33*4 (floats: v11's 27 plus the two new
 * MAX31856_CHANNEL_COUNT-wide rows coupling_tau_s[]/coupling_dead_time_s[],
 * i.e. 27 + 3 + 3 = 33) + 7 (the uint8_t tail, unchanged since v9) + 1 (tail
 * padding to the struct's 4-byte float alignment). Hand-computed the same
 * way as zone_cfg_v11_t's own assert comment -- never sizeof(zone_cfg_t),
 * which by the time this pass lands is already the v13 (tuning_*) shape,
 * not v12's. */
_Static_assert(sizeof(zone_cfg_v12_t) == 156,
               "zone_cfg_v12_t must match the on-flash v12 layout byte-for-byte (156 bytes)"); /* v12 -- predates tuning_* quality fields */

/* Frozen v13 layout -- what zone_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 13->14), tuning_* quality fields and all,
 * predating adaptive_tune_enabled. Same discipline as zone_cfg_v12_t just
 * above: field order hand-copied from v13's actual shape, never derived
 * from the live struct. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    float fuzzy_strength_pct;
    float coupling_coeff[MAX31856_CHANNEL_COUNT];
    float coupling_tau_s[MAX31856_CHANNEL_COUNT];
    float coupling_dead_time_s[MAX31856_CHANNEL_COUNT];
    /* ---- uint8_t tail, exactly as v9/v10/v11/v12 grouped them ---- */
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
    /* ---- ZONES_CFG_VERSION 12->13's tuning-quality record, unchanged by
     * THIS pass -- see zone_cfg_t::tuning_valid's own comment. ---- */
    uint8_t  tuning_valid;
    uint8_t  tuning_method;
    uint8_t  tuning_rule;
    uint8_t  tuning_settled;
    uint8_t  tuning_extrapolation_converged;
    uint8_t  tuning_tau_consistent;
    float    tuning_baseline_c;
    float    tuning_step_ambient_c;
    float    tuning_raw_rise_c;
    float    tuning_rise_inf_c;
    uint32_t tuning_seq;
} zone_cfg_v13_t;

/* 184 = 156's raw (pre-trailing-pad) content, 155 bytes, + 6 (the six new
 * tuning_* uint8_t flags) + 3 (padding to the four tuning_* floats' 4-byte
 * alignment) + 16 (those four floats) + 4 (tuning_seq, already 4-byte
 * aligned) = 155 + 6 + 3 + 16 + 4 = 184, itself already a multiple of 4 so
 * no further trailing pad. Hand-computed the same way as zone_cfg_v12_t's
 * own assert comment -- never sizeof(zone_cfg_t), which by the time this
 * pass lands is already the v14 (adaptive_tune_enabled) shape, not v13's. */
_Static_assert(sizeof(zone_cfg_v13_t) == 184,
               "zone_cfg_v13_t must match the on-flash v13 layout byte-for-byte (184 bytes)"); /* v13 -- predates adaptive_tune_enabled */

/* Frozen v14 layout -- what zone_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 14->15), adaptive_tune_enabled and all, predating
 * coupling_diag_k_dc. Same discipline as zone_cfg_v13_t just above: field
 * order hand-copied from v14's actual shape, never derived from the live
 * struct. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    float fuzzy_strength_pct;
    float coupling_coeff[MAX31856_CHANNEL_COUNT];
    float coupling_tau_s[MAX31856_CHANNEL_COUNT];
    float coupling_dead_time_s[MAX31856_CHANNEL_COUNT];
    /* ---- uint8_t tail, exactly as v9/v10/v11/v12/v13 grouped them ---- */
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
    /* ---- ZONES_CFG_VERSION 12->13's tuning-quality record, unchanged by
     * v13->14 or THIS pass -- see zone_cfg_t::tuning_valid's own comment. ---- */
    uint8_t  tuning_valid;
    uint8_t  tuning_method;
    uint8_t  tuning_rule;
    uint8_t  tuning_settled;
    uint8_t  tuning_extrapolation_converged;
    uint8_t  tuning_tau_consistent;
    float    tuning_baseline_c;
    float    tuning_step_ambient_c;
    float    tuning_raw_rise_c;
    float    tuning_rise_inf_c;
    uint32_t tuning_seq;
    /* ---- ZONES_CFG_VERSION 13->14's adaptive-tune opt-in, unchanged by
     * THIS pass. ---- */
    uint8_t  adaptive_tune_enabled;
} zone_cfg_v14_t;

/* 188 = 184's raw content (184 bytes, itself already a multiple of 4) + 1
 * (adaptive_tune_enabled) rounded back up to 4-byte alignment for the
 * struct's own tail padding = 188. Hand-computed the same way as
 * zone_cfg_v13_t's own assert comment -- never sizeof(zone_cfg_t), which by
 * the time this pass lands is already the v15 (coupling_diag_k_dc) shape,
 * not v14's. */
_Static_assert(sizeof(zone_cfg_v14_t) == 188,
               "zone_cfg_v14_t must match the on-flash v14 layout byte-for-byte (188 bytes)"); /* v14 -- predates coupling_diag_k_dc */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    zone_cfg_v1_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v1_t; /* v1 -- predates continue_on_zone_trip/safety_tc_type */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    zone_cfg_v1_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v2_t; /* v2 */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    zone_cfg_v3_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v3_t; /* v3 */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    zone_cfg_v4_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v4_t; /* v4 */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v5_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v5_t; /* v5 -- adds safety_tc_type */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v7_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v6_t; /* v6 -- v7's zone layout, but no crc32 yet */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v7_t zones[MAX31856_CHANNEL_COUNT];
    uint32_t crc32;
} zones_cfg_v7_t; /* v7 -- adds crc32; predates the v8 override fields */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v8_t zones[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v8_t; /* v8 -- the nine timing overrides lived per-zone; predates timing_profiles[] */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v9_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v9_t; /* v9 -- what zones_cfg_t looked like immediately before this pass;
                    * predates fuzzy_strength_pct/coupling_coeff/coupling_neighbor_zone/
                    * settings_source. zone_timing_profile_t itself is unchanged by this
                    * pass, so it is reused here verbatim rather than frozen again. */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v10_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v10_t; /* v10 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the coupling_coeff[] row. zone_timing_profile_t
                     * unchanged again, reused verbatim same as v9's own comment. */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v11_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v11_t; /* v11 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates coupling_tau_s[]/coupling_dead_time_s[].
                     * zone_timing_profile_t unchanged again, reused verbatim
                     * same as v9/v10's own comment. */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v12_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v12_t; /* v12 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the tuning_* quality fields. zone_timing_profile_t
                     * unchanged again, reused verbatim same as v9/v10/v11's own comment. */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v13_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v13_t; /* v13 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates adaptive_tune_enabled. zone_timing_profile_t
                     * unchanged again, reused verbatim same as v9/v10/v11/v12's own
                     * comment. */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v14_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v14_t; /* v14 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates coupling_diag_k_dc. zone_timing_profile_t
                     * unchanged again, reused verbatim same as v9/v10/v11/v12/v13's
                     * own comment. */


typedef enum {
    ZONES_DECODE_OK,      /* *out is a valid, current-format struct, ready to adopt */
    ZONES_DECODE_CORRUPT, /* reject outright: wrong length for claimed version, unknown
                           * version, CRC mismatch, or failed validate_zones_cfg() --
                           * *out is zeroed, nothing is adopted */
    ZONES_DECODE_NEWER,   /* version > ZONES_CFG_VERSION -- refuse without guessing;
                           * *out is zeroed, but the caller must treat the SOURCE bytes
                           * as real, protected data (see nvs_load_from()'s *out_found) */
} zones_decode_result_t;

/* The one place a stored zones_cfg blob (from NVS or a kiln_cfg_store import)
 * is turned into a trustworthy, current-format zones_cfg_t: a length check
 * against the blob's OWN claimed version before anything is copied or
 * interpreted, typed per-version conversion (never a memcpy of one struct
 * shape over another), raise_heater_timing_to_floors() (see its own comment
 * for why this runs before validation), validate_zones_cfg(), and a CRC
 * check on the current-version path. Used by both zones_http.c's
 * nvs_load_from() and zones_config_import_blob() below -- a blob restored
 * from a kiln_cfg_store slot gets exactly the same scrutiny a blob read off
 * flash does. */
zones_decode_result_t zones_config_json_decode_blob(const void *blob, size_t len, zones_cfg_t *out,
                                                     const char **err_reason);

/* esp_crc32_le() over the struct with crc32 itself zeroed, computed over a
 * local copy so a caller re-validating an already-loaded cfg's crc32 is
 * never mutated by asking. Used by zones_http.c's nvs_save() to stamp a
 * fresh CRC, and internally by zones_config_json_decode_blob() to check one. */
uint32_t zones_config_json_compute_crc(const zones_cfg_t *cfg);

/* Validates every field of `cand` -- a fully migrated, CURRENT-version
 * zones_cfg_t -- against the exact bounds the field parsers below and
 * zones_http.c's zones_config_set_*() setters enforce on a live POST. A
 * config stored by kiln_cfg_store.c may have been saved years ago, under
 * looser bounds, or by firmware this build has since tightened, so it is
 * re-checked here rather than trusted because it was valid once. On
 * failure, *err_reason names the first field that failed. */
bool zones_config_json_validate(const zones_cfg_t *cand, const char **err_reason);

/* Bounded chain walk starting at `start`, following settings_source links
 * through `zones[]` (MAX31856_CHANNEL_COUNT-sized, indexed exactly like
 * zones_cfg_t::zones). Returns true if the chain revisits a zone already on
 * it -- a genuine inheritance cycle -- false if it terminates cleanly. See
 * zones_config_json.c's own copy of this function's original comment (moved
 * verbatim) for the full walk semantics. */
bool zones_config_json_settings_source_chain_has_cycle(const zone_cfg_t zones[MAX31856_CHANNEL_COUNT],
                                                        uint8_t start, uint8_t thermo_count);

/* Every-load fixup, LOAD PATH ONLY: collapses any settings_source cycle in
 * *cfg to ZONE_SETTINGS_SOURCE_CUSTOM on just the zones actually ON the
 * cycle, logging each one by name (`partition`, purely for the log line). A
 * cycle can only reach flash via firmware that predates the chain-walk
 * guards, or direct NVS tampering; either way a config that was valid before
 * this guard shipped must keep booting, not get wiped -- see
 * zones_config_json.c's own copy of this function's original comment for
 * the full reasoning and the index-order-independence argument. */
void zones_config_json_normalize_settings_source_cycles(zones_cfg_t *cfg, const char *partition);

/* Parses one application/x-www-form-urlencoded field from `body`: `key`'s
 * value must be present, parse entirely as a base-10 integer (trailing
 * garbage after a valid numeric prefix is refused, not truncated), and fall
 * within [min, max]. Returns false (leaving *out untouched) on any failure. */
bool zones_config_json_parse_u8_field(const char *body, const char *key, long min, long max, uint8_t *out);

/* Same contract as zones_config_json_parse_u8_field() above, for a float
 * field -- NaN is rejected, inf is caught incidentally by the finite
 * min/max bounds. */
bool zones_config_json_parse_float_field(const char *body, const char *key, float min, float max, float *out);

/* Was `key` present in `body` at all? http_form_find_field() returns >0 for
 * a real value, 0 for a present-but-empty "key=", -2 when the value is
 * longer than the probe buffer, and -1 only when the key is genuinely
 * absent -- this reports the last case only, since 0/-2 both mean "answer
 * 200 to a blank/over-long value and keep the old number" would be a silent
 * data loss for the several zone_cfg_t fields that preserve-on-omit. */
bool zones_config_json_field_present(const char *body, const char *key);

/* Parses tp<p>_name and its nine timing fields into *tp -- the POST
 * /api/zones authority for zone_timing_profile_t. Every field is REQUIRED
 * (unlike the zone fields these nine replaced): there is no legacy client to
 * stay compatible with here, since no firmware before ZONES_CFG_VERSION 8->9
 * ever had a "timing profile" concept to send or omit. Nothing is written to
 * *tp on any rejection path partway through. The caller must already have
 * confirmed tp<p>_name is present before calling this (that is how it
 * decided profile slot p is part of the submission at all) -- this function
 * re-reads the name field itself rather than taking it as a parameter. */
bool zones_config_json_parse_timing_profile_fields(const char *body, uint8_t p, zone_timing_profile_t *tp,
                                                    const char **err_reason);

#ifdef __cplusplus
}
#endif

#endif // ZONES_CONFIG_JSON_H
