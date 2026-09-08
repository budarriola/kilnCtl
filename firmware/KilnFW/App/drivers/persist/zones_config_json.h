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
#include "zones_config_accessors.h" /* ZONE_NAME_MAX_LEN, TIMING_PROFILE_NAME_MAX_LEN, and
                         * every per-field bound (ZONE_*_MAX/MIN) validate_zones_cfg() and
                         * the field parsers below check against -- moved out of zones_http.h
                         * (HW_ABSTRACTION.md item 1, 2026-09-05) */

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever zones_cfg_t's on-flash layout changes -- see
 * zones_config_json.c's decode_zones_blob()/convert_versioned_blob_to_current()
 * and zones_http.c's nvs_save()/nvs_load_from(), which stamp/check this same
 * value. Shared because both files must agree on what "the current version"
 * means: nvs_save() writes it, decode_zones_blob() decides whether a stored
 * blob needs migrating against it. */
#define ZONES_CFG_VERSION 23

/* Bounds for zones_cfg_t::ease_off_window_mult (ZONES_CFG_VERSION 15->16,
 * 2026-09-03): the terminal ease-off's window, as a multiple of a zone's own
 * identified dead time -- see profile_executor_feedforward.c's
 * zone_taper_climb_rate(). This is the field that used to be the compile-
 * time PROFILE_EXECUTOR_EASE_OFF_WINDOW_MULT #define (2.0f), made runtime-
 * configurable so the 2.0x sizing -- picked against a simulator later found
 * to undershoot dwell-entry overshoot -- can be A/B tested on real hardware
 * without a reflash between arms.
 *
 * MIN 0.1f: anything smaller (but nonzero -- see 0's own carve-out just
 * below) collapses the taper to a sliver of the zone's own dead time (a few
 * seconds even for the longest zone here), which is functionally "no
 * ease-off" rather than a deliberately narrow one -- below this the
 * mechanism zone_taper_climb_rate() implements stops doing anything a
 * genuine A/B arm would want to call an ease-off at all.
 *
 * MAX 10.0f: at the longest identified dead time on this board (z0,
 * ~52.8s), 10x is a ~528s (~8.8 minute) taper window -- already long enough
 * to swallow most short dwell segments' entire ramp-in, which is the
 * failure mode an enormous multiplier produces (feedforward climb tapered
 * to near-zero for the whole segment, leaving the PID to make up all of it
 * on its own -- exactly what feedforward exists to avoid). No campaign this
 * A/B is meant to run needs to explore further than that.
 *
 * 0.0f is a SEPARATE, always-legal sentinel outside [MIN, MAX] -- same
 * "0 = use the firmware default" convention as pc_link_abort_silence_ms
 * just above this field in zones_cfg_t, not the same thing as an in-range
 * value near zero. zones_config_json_validate() accepts it explicitly (see
 * that function's own check) and zones_config_get_ease_off_window_mult()
 * substitutes ZONE_EASE_OFF_WINDOW_MULT_DEFAULT for it, the same
 * "never let a divide-by-effectively-nothing window reach zone_taper_climb_
 * rate()" guarantee zone_taper_climb_rate()'s own comment on non-positive
 * windows describes -- 0 never reaches that division; the getter is what
 * turns it into 2.0 first. */
#define ZONE_EASE_OFF_WINDOW_MULT_MIN 0.1f
#define ZONE_EASE_OFF_WINDOW_MULT_MAX 10.0f
/* Unchanged default/migration value -- see zones_cfg_t::ease_off_window_mult's
 * own comment and convert_versioned_blob_to_current()'s case 15 in
 * zones_config_json.c: every existing board upgrading from v15 (or earlier)
 * must get EXACTLY this, matching the removed #define's own value, so
 * behaviour is identical on every board until an operator deliberately
 * changes it. Also what the 0 sentinel above resolves to. */
#define ZONE_EASE_OFF_WINDOW_MULT_DEFAULT 2.0f

/* Bounds for zone_cfg_t::approach_rate_cap_c_per_hr (ZONES_CFG_VERSION 17->18,
 * 2026-09-04, PID_EXPANSION_PLAN.md sec 3.6d, PER_ZONE_TARGET_DESIGN_STUDY.md
 * option (b)): a per-zone ceiling on how fast THIS zone's own commanded PID/
 * feedforward setpoint (profile_executor_internal.h's zone_runtime_t::
 * effective_target_c) may approach the shared, board-wide ramp destination
 * s_exec.target_c. The destination itself, segment-advance, ramp-lock, and
 * feasibility are ALL untouched by this field -- see this field's own
 * "no wire-protocol/ramp-lock change" note below.
 *
 * 0.0f is a SEPARATE, always-legal sentinel meaning "uncapped" -- NOT the
 * same convention as ease_off_window_mult's 0 (which substitutes a firmware
 * DEFAULT cap value). There is no sensible non-zero default rate to
 * substitute here: the correct "no cap" behaviour is for effective_target_c
 * to track s_exec.target_c exactly, every tick, with no rate limit at all --
 * precisely today's behaviour, unchanged. zones_config_get_approach_rate_cap()
 * reports this sentinel back verbatim (0 means "off"), it does not resolve it
 * into some other in-range number the way zones_config_get_ease_off_window_
 * mult() resolves its own 0 into 2.0.
 *
 * MIN 1.0f (C/hr): anything smaller effectively never reaches the segment's
 * own target within any realistic firing duration (at 0.5 C/hr, closing even
 * a 5C gap takes 10 hours) -- a value that low is functionally "never dwell,"
 * not a deliberately slow approach, and is far more likely to be a units
 * mistake (C/min instead of C/hr) than an intentional choice. Refused, never
 * clamped, same discipline as every other setter in this codebase.
 *
 * MAX matches ZONE_MAX_RAMP_C_PER_HR_MAX -- a cap can only ever TIGHTEN a
 * segment's ramp rate (see this field's own "never loosens" guarantee just
 * below), so it can never usefully exceed the fastest ramp a segment could
 * ever command in the first place; anything above that ceiling is a no-op
 * indistinguishable from uncapped and is refused as almost certainly a typo
 * (an operator meaning to type a cap and instead entering, e.g., a
 * temperature). */
#define ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN 1.0f
#define ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX ZONE_MAX_RAMP_C_PER_HR_MAX

/* Bounds for zone_cfg_t::error_band_c / ::rate_band_c_per_s (ZONES_CFG_
 * VERSION 18->19, 2026-09-04, PID_EXPANSION_PLAN.md sec 3.6g): the
 * triangular-membership half-widths pid_fuzzy.c's pid_fuzzy_adjust() uses on
 * its error/error-rate axes -- see pid_fuzzy.c's own ERROR_BAND_C_DEFAULT/
 * RATE_BAND_C_PER_S_DEFAULT comment for the full rationale (why these were
 * compile-time constants, what logs/coupling/fuzzy_bands_envelope_20260904e_
 * report.md found, and why the owner's decision on whether/how to rescale
 * them is out of scope for this pass -- this pass only removes the reflash
 * that decision used to cost).
 *
 * Same "0 = use the firmware default" sentinel convention as
 * ease_off_window_mult (NOT approach_rate_cap_c_per_hr's "0 = off with no
 * substitute" convention -- there is no meaningful "no band" answer for a
 * membership function the way there is a meaningful "no cap" answer for a
 * rate limiter; some finite band width always applies). zones_config_json_
 * validate() accepts 0 explicitly, and zones_config_get_error_band_c()/
 * zones_config_get_rate_band_c_per_s() substitute ZONE_ERROR_BAND_C_DEFAULT/
 * ZONE_RATE_BAND_C_PER_S_DEFAULT for it, at read time, same as
 * zones_config_get_ease_off_window_mult() does for its own field.
 *
 * error_band_c MIN 1.0f / MAX 100.0f: the measured envelope
 * (fuzzy_bands_envelope_20260904e's peak of 5.72 degC) sits comfortably
 * inside this range, as does the unchanged 20.0 default and a plausible
 * rescale toward the report's ~6-8 degC recommendation; 1.0 is small enough
 * that a caller who genuinely wants the fuzzy layer to leave its centre cell
 * on ordinary noise can ask for that (not this pass's call to forbid), while
 * anything below it collapses to a width no real kiln thermocouple noise
 * floor could stay inside, and 100.0 is far past any error this board's own
 * guards would tolerate before tripping (guard_runaway_margin_c and
 * max_temp_c/min_temp_c bound the physically survivable range well below
 * that) -- a value near the ceiling is far more likely a units slip than a
 * deliberate choice.
 *
 * rate_band_c_per_s MIN 0.01f / MAX 5.0f: 0.01 is below the slowest
 * meaningful zone_taper/ramp signal this board's own d_filtered can report
 * without drowning in sensor quantization noise; 5.0 is an order of
 * magnitude past the already-generous 0.5 default (itself ~6x this kiln's
 * fastest commanded ramp, see pid_fuzzy.c) and well past anything but a
 * thermocouple that has come unstuck -- a value that high stops
 * distinguishing "disturbance" from "everything," which is the same failure
 * mode the original RATE_BAND_C_PER_S rescale (0.05 -> 0.5) fixed once
 * already. */
#define ZONE_ERROR_BAND_C_MIN 1.0f
#define ZONE_ERROR_BAND_C_MAX 100.0f
#define ZONE_ERROR_BAND_C_DEFAULT 20.0f
#define ZONE_RATE_BAND_C_PER_S_MIN 0.01f
#define ZONE_RATE_BAND_C_PER_S_MAX 5.0f
#define ZONE_RATE_BAND_C_PER_S_DEFAULT 0.5f

/* MAX31856 CR1.TC[3:0] nibble values 0x00-0x07 name a real thermocouple type
 * (B/E/J/K/N/R/S/T); 0x08-0x0F are the part's voltage-input modes, not
 * thermocouples. This operator-facing config rejects anything past
 * THERMO_TC_T -- see zones_http.c's original comment on this macro (git
 * history) for the full rationale; the raw UART debug path
 * (MAX31856_configure()) deliberately accepts the wider 0-0x0F range and is
 * untouched by this bound. */
#define ZONE_TC_TYPE_MAX_REAL THERMO_TC_T

/* relay_type_t (relay_cycles.h: RELAY_TYPE_SSR=0/CONTACTOR=1/MERCURY=2) as
 * stored on zone_cfg_t below (ZONES_CFG_VERSION 19->20,
 * RELAY_LIFE_BUDGET.md). 0 (ssr) is both the enum's own zero
 * value and this field's migration default -- see zone_cfg_t::relay_type's
 * own comment. */
#define ZONE_RELAY_TYPE_MAX 2

/* thermal_guard_cfg_t::progress_band_c (ZONES_CFG_VERSION 21->22,
 * docs/audits/consumer_without_producer_2026-09-06.md finding 1): guard 1's
 * arrival band, mirrored into zone_cfg_t with the exact "0 = use the
 * firmware default" convention error_band_c/rate_band_c_per_s already use
 * (thermal_guard.c's effective_f() does the substitution, same as every
 * other guard threshold) -- NOT approach_rate_cap_c_per_hr's "0 = off"
 * convention, since there is no "no band" answer guard 1's arrival test can
 * accept. DEFAULT matches thermal_guard.c's PROGRESS_BAND_C compile-time
 * constant exactly, so an unconfigured zone's behaviour is unchanged by this
 * field's introduction. MIN/MAX bound a genuine degrees-C band the same way
 * ZONE_ERROR_BAND_C_MIN/MAX do for the fuzzy layer's error axis -- 0.5 is
 * comfortably below PROFILES.md's documented 3 C default without being
 * indistinguishable from sensor noise, and 20.0 is far past any setpoint
 * offset a healthy PID loop should ever settle at. */
#define ZONE_PROGRESS_BAND_C_MIN 0.5f
#define ZONE_PROGRESS_BAND_C_MAX 20.0f
#define ZONE_PROGRESS_BAND_C_DEFAULT 3.0f

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
    /* Section 3.5's UI-only provenance marker, one byte PER MIRRORABLE GROUP
     * since ZONES_CFG_VERSION 20->21 (docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs) instead of a
     * single whole-zone byte: settings_source[SRC_GROUP_LIMITS],
     * [SRC_GROUP_RELAY_TIMING], [SRC_GROUP_CONTROL], [SRC_GROUP_GUARDS] and
     * [SRC_GROUP_TC] (see those #defines, zones_config_accessors.h, for
     * exactly which fields each group covers). Each entry is independently
     * ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) = "this zone's own settings for
     * that group", otherwise the index of the zone that group's dropdown
     * claims to copy. Measured fields (model_*, coupling_*, tuning_*) and
     * topology fields (name, relay_mask, thermo_mask, ct_mask, relay_type)
     * are not covered by any group and never mirror. Stored ONLY so the
     * settings page re-opens showing the right dropdown state per group --
     * the resolved values are written into each zone's own fields on save,
     * so NOTHING in the control loop ever reads this array. Note 0 is a
     * real value here ("copies zone 0"), not an empty default: see
     * convert_zone_v9() and the v20->v21 migration (every group starts as a
     * copy of the old single byte, not a fresh CUSTOM). */
    uint8_t settings_source[SRC_GROUP_COUNT];
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
    /* ---- ZONES_CFG_VERSION 16->17 (2026-09-04, PID_EXPANSION_PLAN.md
     * sec 3.6d follow-up: "an A/B override needs per-zone reach"). Was
     * zones_cfg_t's own single global scalar (see this field's own
     * z0_dwell_overshoot_mechanism_20260904_report.md-driven move below) --
     * z0's dwell-entry overshoot (~2.2 degC, the largest remaining tracking
     * error on this board) correlates near 1:1 with ramp rate into the
     * transition while z0's own duty is already near zero at the overshoot
     * peak, and z0 carries the longest identified dead time and the
     * heaviest incoming cross-zone coupling of the three zones -- a single
     * board-wide multiplier cannot give z0 a wider taper window without
     * moving z1/z2's too, confounding any A/B run across zones that do not
     * need the same answer.
     *
     * Same "0 = use the firmware default" sentinel convention the removed
     * global field had -- see ZONE_EASE_OFF_WINDOW_MULT_MIN/MAX/DEFAULT's
     * own comment, unchanged by this move: 0 is still always legal, and
     * zone_taper_climb_rate() still never sees a non-positive window.
     * Appended at zone_cfg_t's own true tail, the same safe-growth spot
     * coupling_diag_k_dc used at v14->v15 just above -- so a v16 board's
     * on-flash zone_cfg_v16_t layout (frozen below) stays an exact byte-
     * for-byte prefix of this shape, not a reinterpretation of it. */
    float ease_off_window_mult;
    /* ---- ZONES_CFG_VERSION 17->18 (2026-09-04, PID_EXPANSION_PLAN.md
     * sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md option (b): "a per-zone
     * approach-rate CAP that can only ever TIGHTEN the shared segment ramp
     * rate"). z0's dwell-entry overshoot correlates near 1:1 with ramp rate
     * into the transition; the design study found the shared `target_c`
     * scalar itself does not need to move (which would have redefined
     * ramp-lock/segment-advance/feasibility, PER_ZONE_TARGET_DESIGN_STUDY.md
     * section 2) -- only each zone's own RATE of approach toward that shared
     * destination needs to be individually clampable.
     *
     * 0.0f = uncapped, i.e. this zone's effective setpoint tracks
     * s_exec.target_c exactly, every tick, with zero rate limit -- BIT-
     * IDENTICAL to every firing before this field existed. This is the only
     * legal value below ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN; see that
     * macro's own comment for why 0 is a sentinel here, not merely the
     * bottom of the valid range (unlike ease_off_window_mult's 0, this one
     * does NOT resolve to a substituted default -- "no cap" has no other
     * value that means the same thing).
     *
     * A non-zero value is a ceiling, degC/hr, on how fast
     * zone_runtime_t::effective_target_c (profile_executor_internal.h) may
     * move toward the shared s_exec.target_c -- it can only ever make this
     * zone's own commanded approach SLOWER than the segment's programmed
     * ramp_c_per_hr, never faster: profile_executor.c rate-limits
     * effective_target_c's per-tick step to min(|s_exec.target_c -
     * effective_target_c| implied by one tick, cap * dt_s/3600), so a cap
     * numerically looser than the segment's own commanded rate is a
     * mathematical no-op (effective_target_c already tracks s_exec.target_c
     * at the segment's own, slower rate, and the cap's wider ceiling is
     * never the binding constraint) -- there is no code path by which this
     * field can make a zone approach FASTER than the segment says.
     *
     * Reaches the control loop as this zone's OWN commanded setpoint for its
     * feedforward and PID terms (profile_executor_pid_tick.c) and, per the
     * design study's section 2.4 resolution of the "fake setpoint" bug class
     * (project_autotune_feeds_fake_setpoint.md), also as the value fed to
     * thermal_guard_input_t.setpoint_c for this zone -- an uncapped zone's
     * guard-visible setpoint is therefore unchanged (still s_exec.target_c,
     * since effective_target_c == s_exec.target_c whenever the cap is 0),
     * and a capped zone's guard now sees what it is ACTUALLY being asked to
     * do this tick, not the group's eventual destination.
     *
     * Deliberately untouched by this field, all per the design study's
     * section 4.1: s_exec.target_c itself (the shared destination),
     * segment-advance (still keyed off s_exec.target_c == seg->target_c),
     * ramp-lock (still compares each zone's actual_c against the one shared
     * s_exec.target_c, exactly as before), profile_segment_feasibility()
     * (never reads this field), and safety_link_frames.c's wire setpoint
     * (still `pstat.target_c`, the shared destination -- SaftyFW's S2 guard
     * already reduces per-zone setpoints with max() and needs no change
     * either way, but this option does not even exercise that path since
     * the wire value never becomes per-zone).
     *
     * Bounded by [ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN,
     * ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX] or exactly 0.0f -- refused, never
     * clamped, same discipline as ease_off_window_mult and every other
     * setter in this codebase. Appended at zone_cfg_t's own true tail, the
     * same safe-growth spot ease_off_window_mult used at v16->v17 just
     * above -- so a v17 board's on-flash zone_cfg_v17_t layout (frozen
     * below) stays an exact byte-for-byte prefix of this shape. */
    float approach_rate_cap_c_per_hr;
    /* ---- ZONES_CFG_VERSION 18->19 (2026-09-04, PID_EXPANSION_PLAN.md sec
     * 3.6g: "the bands should be a config change, not a firmware change").
     * pid_fuzzy.c's two triangular-membership half-widths, promoted from
     * ERROR_BAND_C/RATE_BAND_C_PER_S compile-time #defines to per-zone
     * config -- see ZONE_ERROR_BAND_C_MIN/MAX/DEFAULT's own comment just
     * above for the full rationale (the measured-envelope finding that
     * motivated this, the bounds, and why the actual rescale decision is
     * explicitly NOT made by this pass).
     *
     * 0.0f in either field is the "use the firmware default" sentinel, same
     * convention as ease_off_window_mult (NOT approach_rate_cap_c_per_hr's
     * "0 = off" convention -- there is no "no band" state for a membership
     * function). zones_config_get_error_band_c()/zones_config_get_rate_
     * band_c_per_s() resolve 0 (and anything outside [MIN, MAX]) into
     * ZONE_ERROR_BAND_C_DEFAULT/ZONE_RATE_BAND_C_PER_S_DEFAULT at read time
     * -- 20.0f and 0.5f, bit-identical to the removed #defines' own values,
     * so every existing board migrates to EXACTLY today's fuzzy-PID
     * behaviour with no operator action required.
     *
     * Per-zone, not board-wide: these bands describe the fuzzy layer's
     * expectation of THIS zone's own plant (how far its error/rate can
     * drift before a value should be called "large"), and this board's
     * three zones do not share one plant -- z0's model_k_dc runs ~23%
     * higher than its peers and its identified dead time is 21-56% longer
     * (coupling_matrix_resolved.md). A genuine disturbance looks different,
     * in both magnitude and rate, on z0 than on z1/z2, the same reasoning
     * that already justified giving z0 its own reach on ease_off_window_
     * mult and approach_rate_cap_c_per_hr above rather than a shared
     * board-wide scalar for either of those.
     *
     * Bounded by [ZONE_ERROR_BAND_C_MIN, ZONE_ERROR_BAND_C_MAX] /
     * [ZONE_RATE_BAND_C_PER_S_MIN, ZONE_RATE_BAND_C_PER_S_MAX] or exactly
     * 0.0f -- refused, never clamped, same discipline as every other
     * setter in this codebase. Appended at zone_cfg_t's own true tail, the
     * same safe-growth spot approach_rate_cap_c_per_hr used at v17->v18
     * just above -- so a v18 board's on-flash zone_cfg_v18_t layout (frozen
     * below) stays an exact byte-for-byte prefix of this shape. */
    float error_band_c;
    float rate_band_c_per_s;
    /* ---- ZONES_CFG_VERSION 19->20 (2026-09-06, RELAY_LIFE_BUDGET.md
     * step 2): which contact-life budget this zone's relay(s) are rated for.
     * relay_type_t (relay_cycles.h) stored as a plain uint8_t, same
     * convention control_mode/tc_type/etc. already use. relay_mask lets a
     * zone drive several physical relays at once; they switch together as
     * one group (relay_cycles_add() already sums by mask, not per bit), so
     * the type is naturally per zone rather than per relay -- the counter
     * itself stays per relay (relay_cycles.c's RELAY_CYCLES_COUNT slots),
     * this field just tells zones_config_store.c which rated-life table
     * entry to push into every relay named in relay_mask via
     * relay_cycles_set_type(), on load and on every successful save (see
     * that call site's own comment).
     *
     * 0 = RELAY_TYPE_SSR, the "no rated-life budget" default -- the same
     * "0 is always a safe, meaningful value" property control_mode/tc_type/
     * thermo_mask already have, and exactly what an EE2-12NUH heater relay
     * on this board's stock zones already is. Bounded by
     * [0, ZONE_RELAY_TYPE_MAX] -- refused, never clamped, same discipline as
     * every other setter in this codebase (parse_zone_fields() rejects an
     * out-of-range value with a specific reason rather than silently
     * substituting SSR).
     *
     * Appended at zone_cfg_t's own true tail, the same safe-growth spot
     * error_band_c/rate_band_c_per_s used at v18->v19 just above -- so a v19
     * board's on-flash zone_cfg_v19_t layout (frozen below) stays an exact
     * byte-for-byte prefix of this shape. Brand new field, no prior global
     * or per-zone opinion to carry forward (same shape as
     * approach_rate_cap_c_per_hr's v17->v18 migration, not
     * ease_off_window_mult's v16->v17 "carry the removed global verbatim"
     * shape): every migrated zone of every upgrading board lands on the 0
     * (ssr) sentinel via convert_versioned_blob_to_current()'s entry
     * memset, which is already today's real, correct answer for every
     * existing board's heater relays. */
    uint8_t relay_type;
    /* ---- ZONES_CFG_VERSION 21->22 (2026-09-06, docs/audits/
     * consumer_without_producer_2026-09-06.md finding 1): thermal_guard_
     * cfg_t::progress_band_c was read by thermal_guard.c's effective_f()
     * but never set at either build site (profile_executor_run.c,
     * autotune_engine.c) and had no accessor at all -- every zone silently
     * ran guard 1's arrival band on the PROGRESS_BAND_C firmware constant
     * with no way for an operator to override it, despite PROGRESS.md
     * documenting it as tunable. See ZONE_PROGRESS_BAND_C_MIN/MAX/DEFAULT's
     * own comment above for the bounds and the 0-is-the-firmware-default
     * convention.
     *
     * Appended at zone_cfg_t's own true tail, the same safe-growth spot
     * relay_type used at v19->v20 just above -- so a v21 board's on-flash
     * zone_cfg_v21_t layout (frozen below) stays an exact byte-for-byte
     * prefix of this shape. Brand new field, no prior global or per-zone
     * opinion to carry forward: every migrated zone of every upgrading
     * board lands on the 0 (use PROGRESS_BAND_C) sentinel via
     * convert_versioned_blob_to_current()'s entry memset, which is already
     * today's real, correct behaviour for every existing board. */
    float progress_band_c;
    /* ---- ZONES_CFG_VERSION 22->23 (2026-09-07, docs/ON_OFF_ZONE_PLAN.md
     * step 1, two owner decisions: "extend the existing mechanism" -- this
     * is a NEW tail-append, not a reuse of PROFILE_SEG_KIND_RELAY_IO, which
     * is a one-shot per-segment timeline event on a non-zone output and
     * cannot express a state-driven, re-evaluated-every-tick zone type --
     * and "fail-safe default OFF, with a per-zone confirm-gated opt-in to
     * ON" -- see failsafe_state below.
     *
     * zone_type: ZONE_TYPE_HEATER = 0 (zones_config_accessors.h) is the
     * migration default for every existing zone and the zero-initialized
     * default for a fresh/partial config, identical in shape to relay_type's
     * own v19->v20 migration -- every migrated/fresh zone keeps behaving
     * exactly as it does today. Deliberately not zone_control_mode_t (see
     * that enum's own comment): mode answers "how is duty computed", type
     * answers "is this a heat source at all", and overloading one field for
     * both would make every `mode >= PID` test in the tree silently
     * correct-looking while making the coupling matrix's meaning depend on a
     * field it never reads today. */
    uint8_t zone_type;
    /* failsafe_state: 0 = OFF (default), 1 = ON. ON/OFF_ZONE_PLAN.md sec 5:
     * the default MUST stay OFF -- a zero-initialized struct (a fresh save,
     * a partial form, a migrated v22 blob) must never leave a relay
     * energised with nothing owning it, same reasoning as
     * io_leave_on_at_end's own 0 default. An owner who wants a fail-safe-ON
     * device (e.g. a vent that should dump heat on a trip) sets this
     * explicitly through a UI that requires a confirmation for that specific
     * choice -- not implemented this pass (step 1 is schema + safety
     * exclusions only), but the field's storage and default are pinned now
     * so no later pass has to touch the schema again for it. */
    uint8_t failsafe_state;
    /* hyst_c: temperature hysteresis for a future trigger evaluator (plan
     * sec 3), 0 -> 2.0f default at read, same "0 is always a safe, must-
     * substitute" convention as progress_band_c just above. Unused by any
     * consumer this pass -- see zones_config_json.c's own comment on why a
     * pure schema step ships with no reader yet. */
    float hyst_c;
    /* min_on_s/min_off_s: minimum on/off dwell for the same future trigger
     * evaluator, 0 -> 30 default at read. Unused by any consumer this pass,
     * same reasoning as hyst_c above. */
    uint16_t min_on_s;
    uint16_t min_off_s;
} zone_cfg_t;

/* Frozen v22 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 22->23): predates zone_type/failsafe_state/
 * hyst_c/min_on_s/min_off_s. Field order hand-copied from v22's actual
 * shape, never derived from the live struct -- same discipline as every
 * other frozen zone_cfg_vN_t in this file, and closes the one review found
 * missing (docs/audits/): every prior frozen snapshot has a `_Static_assert`
 * pinning its size; this is v22's. */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source[SRC_GROUP_COUNT];
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
    float ease_off_window_mult;
    float approach_rate_cap_c_per_hr;
    float error_band_c;
    float rate_band_c_per_s;
    uint8_t relay_type;
    float progress_band_c;
} zone_cfg_v22_t;

/* 220 = 216 (zone_cfg_v21_t's own byte-for-byte size) + 4 (progress_band_c),
 * a pure tail append with no new padding since a float lands on its own
 * natural 4-byte alignment straight after relay_type. Confirmed against a
 * standalone layout replica of this exact struct, never sizeof(zone_cfg_t) --
 * see zone_cfg_v21_t's own assert comment for why that name is never safe to
 * use for this purpose. */
_Static_assert(sizeof(zone_cfg_v22_t) == 220,
               "zone_cfg_v22_t must match the on-flash v22 layout byte-for-byte (220 bytes)"); /* v22 -- predates zone_type */

/* Per-field offsetof assertions for zone_cfg_v22_t -- same rationale as
 * zone_cfg_v21_t's own block below it. */
_Static_assert(offsetof(zone_cfg_v22_t, name) == 0,
               "zone_cfg_v22_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v22_t, cal_offset_c) == 16,
               "zone_cfg_v22_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v22_t, relay_mask) == 148,
               "zone_cfg_v22_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v22_t, control_mode) == 149,
               "zone_cfg_v22_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v22_t, settings_source) == 154,
               "zone_cfg_v22_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v22_t, coupling_diag_k_dc) == 192,
               "zone_cfg_v22_t::coupling_diag_k_dc must stay at byte offset 192");
_Static_assert(offsetof(zone_cfg_v22_t, ease_off_window_mult) == 196,
               "zone_cfg_v22_t::ease_off_window_mult must stay at byte offset 196");
_Static_assert(offsetof(zone_cfg_v22_t, approach_rate_cap_c_per_hr) == 200,
               "zone_cfg_v22_t::approach_rate_cap_c_per_hr must stay at byte offset 200");
_Static_assert(offsetof(zone_cfg_v22_t, error_band_c) == 204,
               "zone_cfg_v22_t::error_band_c must stay at byte offset 204");
_Static_assert(offsetof(zone_cfg_v22_t, rate_band_c_per_s) == 208,
               "zone_cfg_v22_t::rate_band_c_per_s must stay at byte offset 208");
_Static_assert(offsetof(zone_cfg_v22_t, relay_type) == 212,
               "zone_cfg_v22_t::relay_type must stay at byte offset 212");
_Static_assert(offsetof(zone_cfg_v22_t, progress_band_c) == 216,
               "zone_cfg_v22_t::progress_band_c must stay at byte offset 216");

/* Frozen v21 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 21->22): predates progress_band_c. Field
 * order hand-copied from v21's actual shape, never derived from the live
 * struct -- critically, never the bare `zone_cfg_t` name for this purpose,
 * since that name now refers to the v22 (bigger) shape. */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source[SRC_GROUP_COUNT];
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
    float ease_off_window_mult;
    float approach_rate_cap_c_per_hr;
    float error_band_c;
    float rate_band_c_per_s;
    uint8_t relay_type;
} zone_cfg_v21_t;

/* 216 = 212 (zone_cfg_v20_t's own byte-for-byte size) + 4 (settings_source
 * widened from a single byte to settings_source[SRC_GROUP_COUNT==5], a net
 * +4 bytes with no new padding since the array's tail already lands on the
 * next float's natural 4-byte alignment). Confirmed against a standalone
 * layout replica of this exact struct (same field order/types, no firmware
 * headers involved) rather than guessed -- same discipline as every other
 * frozen zone_cfg_vN_t assert in this file -- never sizeof(zone_cfg_t),
 * which by the time this pass lands is already the v22 shape, not v21's. */
_Static_assert(sizeof(zone_cfg_v21_t) == 216,
               "zone_cfg_v21_t must match the on-flash v21 layout byte-for-byte (216 bytes)"); /* v21 -- predates progress_band_c */

/* Per-field offsetof assertions for zone_cfg_v21_t -- same rationale as
 * zone_cfg_v20_t's own block below it (this is a frozen snapshot pinned
 * against an accidental edit to ITSELF, not a guard against insertion into
 * the live zone_cfg_t; see that comment for the full reasoning and the
 * migration-test coverage that actually catches the live-struct case). */
_Static_assert(offsetof(zone_cfg_v21_t, name) == 0,
               "zone_cfg_v21_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v21_t, cal_offset_c) == 16,
               "zone_cfg_v21_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v21_t, relay_mask) == 148,
               "zone_cfg_v21_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v21_t, control_mode) == 149,
               "zone_cfg_v21_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v21_t, settings_source) == 154,
               "zone_cfg_v21_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v21_t, coupling_diag_k_dc) == 192,
               "zone_cfg_v21_t::coupling_diag_k_dc must stay at byte offset 192");
_Static_assert(offsetof(zone_cfg_v21_t, ease_off_window_mult) == 196,
               "zone_cfg_v21_t::ease_off_window_mult must stay at byte offset 196");
_Static_assert(offsetof(zone_cfg_v21_t, approach_rate_cap_c_per_hr) == 200,
               "zone_cfg_v21_t::approach_rate_cap_c_per_hr must stay at byte offset 200");
_Static_assert(offsetof(zone_cfg_v21_t, error_band_c) == 204,
               "zone_cfg_v21_t::error_band_c must stay at byte offset 204");
_Static_assert(offsetof(zone_cfg_v21_t, rate_band_c_per_s) == 208,
               "zone_cfg_v21_t::rate_band_c_per_s must stay at byte offset 208");
_Static_assert(offsetof(zone_cfg_v21_t, relay_type) == 212,
               "zone_cfg_v21_t::relay_type must stay at byte offset 212");

/* Frozen v20 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 20->21, docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs): a single
 * whole-zone settings_source byte, predating the per-group split
 * (settings_source[SRC_GROUP_COUNT] above). Same discipline as
 * zone_cfg_v19_t below it in this file: field order hand-copied from v20's
 * actual shape, never derived from the live struct -- critically, never the
 * bare `zone_cfg_t` name for this purpose, since that name now refers to
 * the v21 (bigger) shape. */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
    float ease_off_window_mult;
    float approach_rate_cap_c_per_hr;
    float error_band_c;
    float rate_band_c_per_s;
    uint8_t relay_type;
} zone_cfg_v20_t;

/* 212 = 208 (zone_cfg_v19_t's own byte-for-byte size) + 1 (relay_type),
 * padded to 212 for the struct's 4-byte float alignment (209 rounds up).
 * Hand-computed, same discipline as every other frozen zone_cfg_vN_t assert
 * in this file -- never sizeof(zone_cfg_t), which by the time this pass
 * lands is already the v21 shape, not v20's. */
_Static_assert(sizeof(zone_cfg_v20_t) == 212,
               "zone_cfg_v20_t must match the on-flash v20 layout byte-for-byte (212 bytes)"); /* v20 -- predates per-group settings_source */

/* Per-field offsetof assertions for zone_cfg_v20_t -- same rationale as
 * zone_cfg_v19_t's own block below it (this is a frozen snapshot pinned
 * against an accidental edit to ITSELF, not a guard against insertion into
 * the live zone_cfg_t; see that comment for the full reasoning and the
 * migration-test coverage that actually catches the live-struct case). */
_Static_assert(offsetof(zone_cfg_v20_t, name) == 0,
               "zone_cfg_v20_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v20_t, cal_offset_c) == 16,
               "zone_cfg_v20_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v20_t, relay_mask) == 148,
               "zone_cfg_v20_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v20_t, control_mode) == 149,
               "zone_cfg_v20_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v20_t, settings_source) == 154,
               "zone_cfg_v20_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v20_t, coupling_diag_k_dc) == 188,
               "zone_cfg_v20_t::coupling_diag_k_dc must stay at byte offset 188");
_Static_assert(offsetof(zone_cfg_v20_t, ease_off_window_mult) == 192,
               "zone_cfg_v20_t::ease_off_window_mult must stay at byte offset 192");
_Static_assert(offsetof(zone_cfg_v20_t, approach_rate_cap_c_per_hr) == 196,
               "zone_cfg_v20_t::approach_rate_cap_c_per_hr must stay at byte offset 196");
_Static_assert(offsetof(zone_cfg_v20_t, error_band_c) == 200,
               "zone_cfg_v20_t::error_band_c must stay at byte offset 200");
_Static_assert(offsetof(zone_cfg_v20_t, rate_band_c_per_s) == 204,
               "zone_cfg_v20_t::rate_band_c_per_s must stay at byte offset 204");
_Static_assert(offsetof(zone_cfg_v20_t, relay_type) == 208,
               "zone_cfg_v20_t::relay_type must stay at byte offset 208");

/* Frozen v19 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 19->20), error_band_c/rate_band_c_per_s and
 * all, predating relay_type. Same discipline as zone_cfg_v18_t just above it
 * in this file: field order hand-copied from v19's actual shape, never
 * derived from the live struct -- critically, NEVER the bare `zone_cfg_t`
 * name for this purpose, since that name now refers to the v20 (bigger)
 * shape. */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
    float ease_off_window_mult;
    float approach_rate_cap_c_per_hr;
    float error_band_c;
    float rate_band_c_per_s;
} zone_cfg_v19_t;

/* 208 = 200 (zone_cfg_v18_t's own byte-for-byte size) + 4 (2 x float:
 * error_band_c, rate_band_c_per_s), each already 4-byte aligned so no
 * further tail padding. Hand-computed, same discipline as every other
 * frozen zone_cfg_vN_t assert in this file -- never sizeof(zone_cfg_t),
 * which by the time this pass lands is already the v20 shape, not v19's. */
_Static_assert(sizeof(zone_cfg_v19_t) == 208,
               "zone_cfg_v19_t must match the on-flash v19 layout byte-for-byte (208 bytes)"); /* v19 -- predates relay_type */

/* Per-field offsetof assertions for zone_cfg_v19_t -- same rationale as
 * zone_cfg_v18_t's own block above (this is a frozen snapshot pinned against
 * an accidental edit to ITSELF, not a guard against insertion into the live
 * zone_cfg_t; see that comment for the full reasoning and the migration-test
 * coverage that actually catches the live-struct case). */
_Static_assert(offsetof(zone_cfg_v19_t, name) == 0,
               "zone_cfg_v19_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v19_t, cal_offset_c) == 16,
               "zone_cfg_v19_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v19_t, relay_mask) == 148,
               "zone_cfg_v19_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v19_t, control_mode) == 149,
               "zone_cfg_v19_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v19_t, settings_source) == 154,
               "zone_cfg_v19_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v19_t, coupling_diag_k_dc) == 188,
               "zone_cfg_v19_t::coupling_diag_k_dc must stay at byte offset 188");
_Static_assert(offsetof(zone_cfg_v19_t, ease_off_window_mult) == 192,
               "zone_cfg_v19_t::ease_off_window_mult must stay at byte offset 192");
_Static_assert(offsetof(zone_cfg_v19_t, approach_rate_cap_c_per_hr) == 196,
               "zone_cfg_v19_t::approach_rate_cap_c_per_hr must stay at byte offset 196");
_Static_assert(offsetof(zone_cfg_v19_t, error_band_c) == 200,
               "zone_cfg_v19_t::error_band_c must stay at byte offset 200");
_Static_assert(offsetof(zone_cfg_v19_t, rate_band_c_per_s) == 204,
               "zone_cfg_v19_t::rate_band_c_per_s must stay at byte offset 204");

/* Frozen v18 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 18->19), per-zone approach_rate_cap_c_per_hr
 * and all, predating error_band_c/rate_band_c_per_s. Same discipline as
 * zone_cfg_v17_t just above it in this file: field order hand-copied from
 * v18's actual shape, never derived from the live struct -- critically,
 * NEVER the bare `zone_cfg_t` name for this purpose, since that name now
 * refers to the v19 (bigger) shape. */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
    float ease_off_window_mult;
    float approach_rate_cap_c_per_hr;
} zone_cfg_v18_t;

/* 200 = 196 (zone_cfg_v17_t's own byte-for-byte size, per that struct's
 * assert comment) + 4 (approach_rate_cap_c_per_hr, already 4-byte aligned so
 * no further tail padding). Hand-computed, same discipline as every other
 * frozen zone_cfg_vN_t assert in this file -- never sizeof(zone_cfg_t),
 * which by the time this pass lands is already the v19 shape, not v18's. */
_Static_assert(sizeof(zone_cfg_v18_t) == 200,
               "zone_cfg_v18_t must match the on-flash v18 layout byte-for-byte (200 bytes)"); /* v18 -- predates per-zone error_band_c/rate_band_c_per_s */

/* Per-field offsetof assertions for zone_cfg_v18_t. CORRECTION (2026-09-04
 * review): these do NOT guard against a mid-struct insertion into the LIVE
 * zone_cfg_t -- zone_cfg_v18_t is a frozen, hand-copied snapshot that by
 * definition never changes again, so nothing can insert a field into it.
 * What these actually pin is this FROZEN COPY against an accidental edit to
 * itself (a future maintainer "cleaning up" this block, or copy-pasting the
 * wrong version's fields into it) -- if that ever reordered or resized a
 * field here, the sizeof assert above could stay green (same total size)
 * while a per-field offset silently moved; these catch that.
 *
 * The failure mode the original commit message described -- a field
 * inserted mid-struct into the LIVE zone_cfg_t, corrupting
 * zones_config_migrate.c's per-zone memcpy(&out->zones[i], &src.zones[i],
 * sizeof(src.zones[i])) -- is instead caught by the v9..v18 migration tests
 * in test_zones_http.c: each stages an old-version blob and asserts on
 * several zone fields spanning the struct (model_k_dc, coupling_diag_k_dc,
 * ease_off_window_mult, approach_rate_cap_c_per_hr, pid_kp/ki/kd, etc.).
 * Verified directly (2026-09-04): inserting an extra float field into the
 * live zone_cfg_t between model_dead_time_s and fuzzy_strength_pct broke
 * essentially every v9->v19 migration test with "got 0.0000, want <real
 * value>" failures, with none of the zone_cfg_vNN_t offsetof asserts below
 * needing to fire at all. zone_cfg_t itself carries no offsetof asserts of
 * its own on purpose: it legitimately grows every version (a new field is
 * appended at its tail on every bump), so pinning its interior offsets
 * would need a maintenance edit on every single version bump for no
 * additional coverage beyond what the migration tests already give for
 * free. */
_Static_assert(offsetof(zone_cfg_v18_t, name) == 0,
               "zone_cfg_v18_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v18_t, cal_offset_c) == 16,
               "zone_cfg_v18_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v18_t, pid_kp) == 20,
               "zone_cfg_v18_t::pid_kp must stay at byte offset 20");
_Static_assert(offsetof(zone_cfg_v18_t, pid_ki) == 24,
               "zone_cfg_v18_t::pid_ki must stay at byte offset 24");
_Static_assert(offsetof(zone_cfg_v18_t, pid_kd) == 28,
               "zone_cfg_v18_t::pid_kd must stay at byte offset 28");
_Static_assert(offsetof(zone_cfg_v18_t, max_ramp_c_per_hr) == 32,
               "zone_cfg_v18_t::max_ramp_c_per_hr must stay at byte offset 32");
_Static_assert(offsetof(zone_cfg_v18_t, sanity_rate_c_per_min) == 36,
               "zone_cfg_v18_t::sanity_rate_c_per_min must stay at byte offset 36");
_Static_assert(offsetof(zone_cfg_v18_t, max_temp_c) == 40,
               "zone_cfg_v18_t::max_temp_c must stay at byte offset 40");
_Static_assert(offsetof(zone_cfg_v18_t, min_temp_c) == 44,
               "zone_cfg_v18_t::min_temp_c must stay at byte offset 44");
_Static_assert(offsetof(zone_cfg_v18_t, heater_window_ms) == 48,
               "zone_cfg_v18_t::heater_window_ms must stay at byte offset 48");
_Static_assert(offsetof(zone_cfg_v18_t, heater_min_on_ms) == 52,
               "zone_cfg_v18_t::heater_min_on_ms must stay at byte offset 52");
_Static_assert(offsetof(zone_cfg_v18_t, heater_min_off_ms) == 56,
               "zone_cfg_v18_t::heater_min_off_ms must stay at byte offset 56");
_Static_assert(offsetof(zone_cfg_v18_t, guard_wrong_dir_window_s) == 60,
               "zone_cfg_v18_t::guard_wrong_dir_window_s must stay at byte offset 60");
_Static_assert(offsetof(zone_cfg_v18_t, guard_wrong_dir_rate_c_per_min) == 64,
               "zone_cfg_v18_t::guard_wrong_dir_rate_c_per_min must stay at byte offset 64");
_Static_assert(offsetof(zone_cfg_v18_t, guard_off_settle_s) == 68,
               "zone_cfg_v18_t::guard_off_settle_s must stay at byte offset 68");
_Static_assert(offsetof(zone_cfg_v18_t, guard_runaway_rate_c_per_min) == 72,
               "zone_cfg_v18_t::guard_runaway_rate_c_per_min must stay at byte offset 72");
_Static_assert(offsetof(zone_cfg_v18_t, guard_runaway_margin_c) == 76,
               "zone_cfg_v18_t::guard_runaway_margin_c must stay at byte offset 76");
_Static_assert(offsetof(zone_cfg_v18_t, guard_drift_period_s) == 80,
               "zone_cfg_v18_t::guard_drift_period_s must stay at byte offset 80");
_Static_assert(offsetof(zone_cfg_v18_t, guard_sensor_fault_debounce_ticks) == 84,
               "zone_cfg_v18_t::guard_sensor_fault_debounce_ticks must stay at byte offset 84");
_Static_assert(offsetof(zone_cfg_v18_t, guard_frozen_window_s) == 88,
               "zone_cfg_v18_t::guard_frozen_window_s must stay at byte offset 88");
_Static_assert(offsetof(zone_cfg_v18_t, cross_zone_max_delta_c) == 92,
               "zone_cfg_v18_t::cross_zone_max_delta_c must stay at byte offset 92");
_Static_assert(offsetof(zone_cfg_v18_t, model_k_dc) == 96,
               "zone_cfg_v18_t::model_k_dc must stay at byte offset 96");
_Static_assert(offsetof(zone_cfg_v18_t, model_tau_s) == 100,
               "zone_cfg_v18_t::model_tau_s must stay at byte offset 100");
_Static_assert(offsetof(zone_cfg_v18_t, model_dead_time_s) == 104,
               "zone_cfg_v18_t::model_dead_time_s must stay at byte offset 104");
_Static_assert(offsetof(zone_cfg_v18_t, fuzzy_strength_pct) == 108,
               "zone_cfg_v18_t::fuzzy_strength_pct must stay at byte offset 108");
_Static_assert(offsetof(zone_cfg_v18_t, coupling_coeff) == 112,
               "zone_cfg_v18_t::coupling_coeff must stay at byte offset 112");
_Static_assert(offsetof(zone_cfg_v18_t, coupling_tau_s) == 124,
               "zone_cfg_v18_t::coupling_tau_s must stay at byte offset 124");
_Static_assert(offsetof(zone_cfg_v18_t, coupling_dead_time_s) == 136,
               "zone_cfg_v18_t::coupling_dead_time_s must stay at byte offset 136");
_Static_assert(offsetof(zone_cfg_v18_t, relay_mask) == 148,
               "zone_cfg_v18_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v18_t, control_mode) == 149,
               "zone_cfg_v18_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v18_t, tc_type) == 150,
               "zone_cfg_v18_t::tc_type must stay at byte offset 150");
_Static_assert(offsetof(zone_cfg_v18_t, thermo_mask) == 151,
               "zone_cfg_v18_t::thermo_mask must stay at byte offset 151");
_Static_assert(offsetof(zone_cfg_v18_t, ct_mask) == 152,
               "zone_cfg_v18_t::ct_mask must stay at byte offset 152");
_Static_assert(offsetof(zone_cfg_v18_t, timing_profile) == 153,
               "zone_cfg_v18_t::timing_profile must stay at byte offset 153");
_Static_assert(offsetof(zone_cfg_v18_t, settings_source) == 154,
               "zone_cfg_v18_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_valid) == 155,
               "zone_cfg_v18_t::tuning_valid must stay at byte offset 155");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_method) == 156,
               "zone_cfg_v18_t::tuning_method must stay at byte offset 156");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_rule) == 157,
               "zone_cfg_v18_t::tuning_rule must stay at byte offset 157");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_settled) == 158,
               "zone_cfg_v18_t::tuning_settled must stay at byte offset 158");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_extrapolation_converged) == 159,
               "zone_cfg_v18_t::tuning_extrapolation_converged must stay at byte offset 159");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_tau_consistent) == 160,
               "zone_cfg_v18_t::tuning_tau_consistent must stay at byte offset 160");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_baseline_c) == 164,
               "zone_cfg_v18_t::tuning_baseline_c must stay at byte offset 164");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_step_ambient_c) == 168,
               "zone_cfg_v18_t::tuning_step_ambient_c must stay at byte offset 168");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_raw_rise_c) == 172,
               "zone_cfg_v18_t::tuning_raw_rise_c must stay at byte offset 172");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_rise_inf_c) == 176,
               "zone_cfg_v18_t::tuning_rise_inf_c must stay at byte offset 176");
_Static_assert(offsetof(zone_cfg_v18_t, tuning_seq) == 180,
               "zone_cfg_v18_t::tuning_seq must stay at byte offset 180");
_Static_assert(offsetof(zone_cfg_v18_t, adaptive_tune_enabled) == 184,
               "zone_cfg_v18_t::adaptive_tune_enabled must stay at byte offset 184");
_Static_assert(offsetof(zone_cfg_v18_t, coupling_diag_k_dc) == 188,
               "zone_cfg_v18_t::coupling_diag_k_dc must stay at byte offset 188");
_Static_assert(offsetof(zone_cfg_v18_t, ease_off_window_mult) == 192,
               "zone_cfg_v18_t::ease_off_window_mult must stay at byte offset 192");
_Static_assert(offsetof(zone_cfg_v18_t, approach_rate_cap_c_per_hr) == 196,
               "zone_cfg_v18_t::approach_rate_cap_c_per_hr must stay at byte offset 196");

/* Frozen v17 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 17->18), per-zone ease_off_window_mult and
 * all, predating approach_rate_cap_c_per_hr. Same discipline as
 * zone_cfg_v16_t just above it in this file: field order hand-copied from
 * v17's actual shape, never derived from the live struct -- critically,
 * NEVER the bare `zone_cfg_t` name for this purpose, since that name now
 * refers to the v18 (bigger) shape. */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
    float ease_off_window_mult;
} zone_cfg_v17_t;

/* 196 = 192 (zone_cfg_v16_t's own byte-for-byte size, per that struct's
 * assert comment) + 4 (ease_off_window_mult, already 4-byte aligned so no
 * further tail padding). Hand-computed, same discipline as every other
 * frozen zone_cfg_vN_t assert in this file -- never sizeof(zone_cfg_t),
 * which by the time this pass lands is already the v18 shape, not v17's. */
_Static_assert(sizeof(zone_cfg_v17_t) == 196,
               "zone_cfg_v17_t must match the on-flash v17 layout byte-for-byte (196 bytes)"); /* v17 -- predates per-zone approach_rate_cap_c_per_hr */

/* Per-field offsetof assertions for zone_cfg_v17_t -- same corrected purpose
 * as zone_cfg_v18_t's own block above (see its comment for the full
 * explanation): these guard this FROZEN snapshot against an accidental
 * future edit to itself, not a mid-struct insertion into the live
 * zone_cfg_t -- that failure mode is caught by the migration tests instead. */
_Static_assert(offsetof(zone_cfg_v17_t, name) == 0,
               "zone_cfg_v17_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v17_t, cal_offset_c) == 16,
               "zone_cfg_v17_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v17_t, pid_kp) == 20,
               "zone_cfg_v17_t::pid_kp must stay at byte offset 20");
_Static_assert(offsetof(zone_cfg_v17_t, pid_ki) == 24,
               "zone_cfg_v17_t::pid_ki must stay at byte offset 24");
_Static_assert(offsetof(zone_cfg_v17_t, pid_kd) == 28,
               "zone_cfg_v17_t::pid_kd must stay at byte offset 28");
_Static_assert(offsetof(zone_cfg_v17_t, max_ramp_c_per_hr) == 32,
               "zone_cfg_v17_t::max_ramp_c_per_hr must stay at byte offset 32");
_Static_assert(offsetof(zone_cfg_v17_t, sanity_rate_c_per_min) == 36,
               "zone_cfg_v17_t::sanity_rate_c_per_min must stay at byte offset 36");
_Static_assert(offsetof(zone_cfg_v17_t, max_temp_c) == 40,
               "zone_cfg_v17_t::max_temp_c must stay at byte offset 40");
_Static_assert(offsetof(zone_cfg_v17_t, min_temp_c) == 44,
               "zone_cfg_v17_t::min_temp_c must stay at byte offset 44");
_Static_assert(offsetof(zone_cfg_v17_t, heater_window_ms) == 48,
               "zone_cfg_v17_t::heater_window_ms must stay at byte offset 48");
_Static_assert(offsetof(zone_cfg_v17_t, heater_min_on_ms) == 52,
               "zone_cfg_v17_t::heater_min_on_ms must stay at byte offset 52");
_Static_assert(offsetof(zone_cfg_v17_t, heater_min_off_ms) == 56,
               "zone_cfg_v17_t::heater_min_off_ms must stay at byte offset 56");
_Static_assert(offsetof(zone_cfg_v17_t, guard_wrong_dir_window_s) == 60,
               "zone_cfg_v17_t::guard_wrong_dir_window_s must stay at byte offset 60");
_Static_assert(offsetof(zone_cfg_v17_t, guard_wrong_dir_rate_c_per_min) == 64,
               "zone_cfg_v17_t::guard_wrong_dir_rate_c_per_min must stay at byte offset 64");
_Static_assert(offsetof(zone_cfg_v17_t, guard_off_settle_s) == 68,
               "zone_cfg_v17_t::guard_off_settle_s must stay at byte offset 68");
_Static_assert(offsetof(zone_cfg_v17_t, guard_runaway_rate_c_per_min) == 72,
               "zone_cfg_v17_t::guard_runaway_rate_c_per_min must stay at byte offset 72");
_Static_assert(offsetof(zone_cfg_v17_t, guard_runaway_margin_c) == 76,
               "zone_cfg_v17_t::guard_runaway_margin_c must stay at byte offset 76");
_Static_assert(offsetof(zone_cfg_v17_t, guard_drift_period_s) == 80,
               "zone_cfg_v17_t::guard_drift_period_s must stay at byte offset 80");
_Static_assert(offsetof(zone_cfg_v17_t, guard_sensor_fault_debounce_ticks) == 84,
               "zone_cfg_v17_t::guard_sensor_fault_debounce_ticks must stay at byte offset 84");
_Static_assert(offsetof(zone_cfg_v17_t, guard_frozen_window_s) == 88,
               "zone_cfg_v17_t::guard_frozen_window_s must stay at byte offset 88");
_Static_assert(offsetof(zone_cfg_v17_t, cross_zone_max_delta_c) == 92,
               "zone_cfg_v17_t::cross_zone_max_delta_c must stay at byte offset 92");
_Static_assert(offsetof(zone_cfg_v17_t, model_k_dc) == 96,
               "zone_cfg_v17_t::model_k_dc must stay at byte offset 96");
_Static_assert(offsetof(zone_cfg_v17_t, model_tau_s) == 100,
               "zone_cfg_v17_t::model_tau_s must stay at byte offset 100");
_Static_assert(offsetof(zone_cfg_v17_t, model_dead_time_s) == 104,
               "zone_cfg_v17_t::model_dead_time_s must stay at byte offset 104");
_Static_assert(offsetof(zone_cfg_v17_t, fuzzy_strength_pct) == 108,
               "zone_cfg_v17_t::fuzzy_strength_pct must stay at byte offset 108");
_Static_assert(offsetof(zone_cfg_v17_t, coupling_coeff) == 112,
               "zone_cfg_v17_t::coupling_coeff must stay at byte offset 112");
_Static_assert(offsetof(zone_cfg_v17_t, coupling_tau_s) == 124,
               "zone_cfg_v17_t::coupling_tau_s must stay at byte offset 124");
_Static_assert(offsetof(zone_cfg_v17_t, coupling_dead_time_s) == 136,
               "zone_cfg_v17_t::coupling_dead_time_s must stay at byte offset 136");
_Static_assert(offsetof(zone_cfg_v17_t, relay_mask) == 148,
               "zone_cfg_v17_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v17_t, control_mode) == 149,
               "zone_cfg_v17_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v17_t, tc_type) == 150,
               "zone_cfg_v17_t::tc_type must stay at byte offset 150");
_Static_assert(offsetof(zone_cfg_v17_t, thermo_mask) == 151,
               "zone_cfg_v17_t::thermo_mask must stay at byte offset 151");
_Static_assert(offsetof(zone_cfg_v17_t, ct_mask) == 152,
               "zone_cfg_v17_t::ct_mask must stay at byte offset 152");
_Static_assert(offsetof(zone_cfg_v17_t, timing_profile) == 153,
               "zone_cfg_v17_t::timing_profile must stay at byte offset 153");
_Static_assert(offsetof(zone_cfg_v17_t, settings_source) == 154,
               "zone_cfg_v17_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_valid) == 155,
               "zone_cfg_v17_t::tuning_valid must stay at byte offset 155");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_method) == 156,
               "zone_cfg_v17_t::tuning_method must stay at byte offset 156");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_rule) == 157,
               "zone_cfg_v17_t::tuning_rule must stay at byte offset 157");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_settled) == 158,
               "zone_cfg_v17_t::tuning_settled must stay at byte offset 158");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_extrapolation_converged) == 159,
               "zone_cfg_v17_t::tuning_extrapolation_converged must stay at byte offset 159");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_tau_consistent) == 160,
               "zone_cfg_v17_t::tuning_tau_consistent must stay at byte offset 160");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_baseline_c) == 164,
               "zone_cfg_v17_t::tuning_baseline_c must stay at byte offset 164");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_step_ambient_c) == 168,
               "zone_cfg_v17_t::tuning_step_ambient_c must stay at byte offset 168");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_raw_rise_c) == 172,
               "zone_cfg_v17_t::tuning_raw_rise_c must stay at byte offset 172");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_rise_inf_c) == 176,
               "zone_cfg_v17_t::tuning_rise_inf_c must stay at byte offset 176");
_Static_assert(offsetof(zone_cfg_v17_t, tuning_seq) == 180,
               "zone_cfg_v17_t::tuning_seq must stay at byte offset 180");
_Static_assert(offsetof(zone_cfg_v17_t, adaptive_tune_enabled) == 184,
               "zone_cfg_v17_t::adaptive_tune_enabled must stay at byte offset 184");
_Static_assert(offsetof(zone_cfg_v17_t, coupling_diag_k_dc) == 188,
               "zone_cfg_v17_t::coupling_diag_k_dc must stay at byte offset 188");
_Static_assert(offsetof(zone_cfg_v17_t, ease_off_window_mult) == 192,
               "zone_cfg_v17_t::ease_off_window_mult must stay at byte offset 192");

/* Frozen v16 zone layout -- what zone_cfg_t looked like immediately before
 * THIS pass (ZONES_CFG_VERSION 16->17), coupling_diag_k_dc and all,
 * predating the per-zone ease_off_window_mult above. Same discipline as
 * zone_cfg_v14_t just above it in this file: field order hand-copied from
 * v16's actual shape, never derived from the live struct -- critically,
 * NEVER the bare `zone_cfg_t` name for this purpose, since that name now
 * refers to the v17 (bigger) shape; zones_cfg_v15_t below this struct is
 * repointed at this frozen type for exactly that reason (it used to reuse
 * zone_cfg_t verbatim, back when doing so was still correct). */
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
    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t tc_type;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    uint8_t timing_profile;
    uint8_t settings_source;
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
    uint8_t  adaptive_tune_enabled;
    float coupling_diag_k_dc;
} zone_cfg_v16_t;

/* 196 = 188 (zone_cfg_v14_t's own byte-for-byte size, per that struct's
 * assert comment) + 4 (coupling_diag_k_dc, already 4-byte aligned so no
 * further tail padding) = 192, itself already a multiple of 4. zone_cfg_t
 * was UNCHANGED by ZONES_CFG_VERSION 15->16 (that pass's new field was
 * zones_cfg_t's own top-level scalar, not a per-zone one -- see the removed
 * global field's comment, git history), so this is also exactly what a v16
 * board's zone_cfg_t looked like. Hand-computed the same way as every
 * other frozen zone_cfg_vN_t assert in this file -- never sizeof(zone_cfg_t),
 * which by the time this pass lands is already the v17 shape, not v16's. */
_Static_assert(sizeof(zone_cfg_v16_t) == 192,
               "zone_cfg_v16_t must match the on-flash v16 layout byte-for-byte (192 bytes)"); /* v16 -- predates per-zone ease_off_window_mult */

/* Per-field offsetof assertions for zone_cfg_v16_t -- same corrected purpose
 * as zone_cfg_v18_t's own block above (see its comment for the full
 * explanation): these guard this FROZEN snapshot against an accidental
 * future edit to itself, not a mid-struct insertion into the live
 * zone_cfg_t -- that failure mode is caught by the migration tests instead. */
_Static_assert(offsetof(zone_cfg_v16_t, name) == 0,
               "zone_cfg_v16_t::name must stay at byte offset 0");
_Static_assert(offsetof(zone_cfg_v16_t, cal_offset_c) == 16,
               "zone_cfg_v16_t::cal_offset_c must stay at byte offset 16");
_Static_assert(offsetof(zone_cfg_v16_t, pid_kp) == 20,
               "zone_cfg_v16_t::pid_kp must stay at byte offset 20");
_Static_assert(offsetof(zone_cfg_v16_t, pid_ki) == 24,
               "zone_cfg_v16_t::pid_ki must stay at byte offset 24");
_Static_assert(offsetof(zone_cfg_v16_t, pid_kd) == 28,
               "zone_cfg_v16_t::pid_kd must stay at byte offset 28");
_Static_assert(offsetof(zone_cfg_v16_t, max_ramp_c_per_hr) == 32,
               "zone_cfg_v16_t::max_ramp_c_per_hr must stay at byte offset 32");
_Static_assert(offsetof(zone_cfg_v16_t, sanity_rate_c_per_min) == 36,
               "zone_cfg_v16_t::sanity_rate_c_per_min must stay at byte offset 36");
_Static_assert(offsetof(zone_cfg_v16_t, max_temp_c) == 40,
               "zone_cfg_v16_t::max_temp_c must stay at byte offset 40");
_Static_assert(offsetof(zone_cfg_v16_t, min_temp_c) == 44,
               "zone_cfg_v16_t::min_temp_c must stay at byte offset 44");
_Static_assert(offsetof(zone_cfg_v16_t, heater_window_ms) == 48,
               "zone_cfg_v16_t::heater_window_ms must stay at byte offset 48");
_Static_assert(offsetof(zone_cfg_v16_t, heater_min_on_ms) == 52,
               "zone_cfg_v16_t::heater_min_on_ms must stay at byte offset 52");
_Static_assert(offsetof(zone_cfg_v16_t, heater_min_off_ms) == 56,
               "zone_cfg_v16_t::heater_min_off_ms must stay at byte offset 56");
_Static_assert(offsetof(zone_cfg_v16_t, guard_wrong_dir_window_s) == 60,
               "zone_cfg_v16_t::guard_wrong_dir_window_s must stay at byte offset 60");
_Static_assert(offsetof(zone_cfg_v16_t, guard_wrong_dir_rate_c_per_min) == 64,
               "zone_cfg_v16_t::guard_wrong_dir_rate_c_per_min must stay at byte offset 64");
_Static_assert(offsetof(zone_cfg_v16_t, guard_off_settle_s) == 68,
               "zone_cfg_v16_t::guard_off_settle_s must stay at byte offset 68");
_Static_assert(offsetof(zone_cfg_v16_t, guard_runaway_rate_c_per_min) == 72,
               "zone_cfg_v16_t::guard_runaway_rate_c_per_min must stay at byte offset 72");
_Static_assert(offsetof(zone_cfg_v16_t, guard_runaway_margin_c) == 76,
               "zone_cfg_v16_t::guard_runaway_margin_c must stay at byte offset 76");
_Static_assert(offsetof(zone_cfg_v16_t, guard_drift_period_s) == 80,
               "zone_cfg_v16_t::guard_drift_period_s must stay at byte offset 80");
_Static_assert(offsetof(zone_cfg_v16_t, guard_sensor_fault_debounce_ticks) == 84,
               "zone_cfg_v16_t::guard_sensor_fault_debounce_ticks must stay at byte offset 84");
_Static_assert(offsetof(zone_cfg_v16_t, guard_frozen_window_s) == 88,
               "zone_cfg_v16_t::guard_frozen_window_s must stay at byte offset 88");
_Static_assert(offsetof(zone_cfg_v16_t, cross_zone_max_delta_c) == 92,
               "zone_cfg_v16_t::cross_zone_max_delta_c must stay at byte offset 92");
_Static_assert(offsetof(zone_cfg_v16_t, model_k_dc) == 96,
               "zone_cfg_v16_t::model_k_dc must stay at byte offset 96");
_Static_assert(offsetof(zone_cfg_v16_t, model_tau_s) == 100,
               "zone_cfg_v16_t::model_tau_s must stay at byte offset 100");
_Static_assert(offsetof(zone_cfg_v16_t, model_dead_time_s) == 104,
               "zone_cfg_v16_t::model_dead_time_s must stay at byte offset 104");
_Static_assert(offsetof(zone_cfg_v16_t, fuzzy_strength_pct) == 108,
               "zone_cfg_v16_t::fuzzy_strength_pct must stay at byte offset 108");
_Static_assert(offsetof(zone_cfg_v16_t, coupling_coeff) == 112,
               "zone_cfg_v16_t::coupling_coeff must stay at byte offset 112");
_Static_assert(offsetof(zone_cfg_v16_t, coupling_tau_s) == 124,
               "zone_cfg_v16_t::coupling_tau_s must stay at byte offset 124");
_Static_assert(offsetof(zone_cfg_v16_t, coupling_dead_time_s) == 136,
               "zone_cfg_v16_t::coupling_dead_time_s must stay at byte offset 136");
_Static_assert(offsetof(zone_cfg_v16_t, relay_mask) == 148,
               "zone_cfg_v16_t::relay_mask must stay at byte offset 148");
_Static_assert(offsetof(zone_cfg_v16_t, control_mode) == 149,
               "zone_cfg_v16_t::control_mode must stay at byte offset 149");
_Static_assert(offsetof(zone_cfg_v16_t, tc_type) == 150,
               "zone_cfg_v16_t::tc_type must stay at byte offset 150");
_Static_assert(offsetof(zone_cfg_v16_t, thermo_mask) == 151,
               "zone_cfg_v16_t::thermo_mask must stay at byte offset 151");
_Static_assert(offsetof(zone_cfg_v16_t, ct_mask) == 152,
               "zone_cfg_v16_t::ct_mask must stay at byte offset 152");
_Static_assert(offsetof(zone_cfg_v16_t, timing_profile) == 153,
               "zone_cfg_v16_t::timing_profile must stay at byte offset 153");
_Static_assert(offsetof(zone_cfg_v16_t, settings_source) == 154,
               "zone_cfg_v16_t::settings_source must stay at byte offset 154");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_valid) == 155,
               "zone_cfg_v16_t::tuning_valid must stay at byte offset 155");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_method) == 156,
               "zone_cfg_v16_t::tuning_method must stay at byte offset 156");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_rule) == 157,
               "zone_cfg_v16_t::tuning_rule must stay at byte offset 157");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_settled) == 158,
               "zone_cfg_v16_t::tuning_settled must stay at byte offset 158");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_extrapolation_converged) == 159,
               "zone_cfg_v16_t::tuning_extrapolation_converged must stay at byte offset 159");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_tau_consistent) == 160,
               "zone_cfg_v16_t::tuning_tau_consistent must stay at byte offset 160");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_baseline_c) == 164,
               "zone_cfg_v16_t::tuning_baseline_c must stay at byte offset 164");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_step_ambient_c) == 168,
               "zone_cfg_v16_t::tuning_step_ambient_c must stay at byte offset 168");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_raw_rise_c) == 172,
               "zone_cfg_v16_t::tuning_raw_rise_c must stay at byte offset 172");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_rise_inf_c) == 176,
               "zone_cfg_v16_t::tuning_rise_inf_c must stay at byte offset 176");
_Static_assert(offsetof(zone_cfg_v16_t, tuning_seq) == 180,
               "zone_cfg_v16_t::tuning_seq must stay at byte offset 180");
_Static_assert(offsetof(zone_cfg_v16_t, adaptive_tune_enabled) == 184,
               "zone_cfg_v16_t::adaptive_tune_enabled must stay at byte offset 184");
_Static_assert(offsetof(zone_cfg_v16_t, coupling_diag_k_dc) == 188,
               "zone_cfg_v16_t::coupling_diag_k_dc must stay at byte offset 188");


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
    /* ZONES_CFG_VERSION 15->16 (2026-09-03) added a global
     * ease_off_window_mult scalar here -- REMOVED at 16->17 (2026-09-04,
     * PID_EXPANSION_PLAN.md sec 3.6d): a single board-wide multiplier could
     * not give z0 alone a wider terminal ease-off taper window without
     * moving z1/z2's too, and z0_dwell_overshoot_mechanism_20260904_report.md
     * found z0's dwell-entry overshoot needs exactly that kind of isolated
     * change to A/B test. The multiplier now lives per-zone, at zone_cfg_t's
     * own tail (see zone_cfg_t::ease_off_window_mult) -- zones_cfg_v16_t
     * above is the frozen historical layout that still carries this global
     * scalar, for decoding a v16 board's real on-flash blob. */
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

/* Frozen v15 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 15->16), coupling_diag_k_dc and all, predating
 * ease_off_window_mult. zone_cfg_t ITSELF was unchanged by that pass (the new
 * field was a top-level zones_cfg_t field, not a per-zone one), so this
 * snapshot originally reused zone_cfg_t verbatim, the same way v9-v14 above
 * reused zone_timing_profile_t verbatim whenever it did not change --
 * REPOINTED at zone_cfg_v16_t by ZONES_CFG_VERSION 16->17 (this pass), since
 * the bare name `zone_cfg_t` now refers to the v17 shape and would silently
 * stop describing a real v15 board's on-flash bytes otherwise (zone_cfg_v16_t
 * is byte-for-byte identical to what zone_cfg_t was at v15/v16, per that
 * struct's own comment). */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v16_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v15_t; /* v15 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates ease_off_window_mult. zone_timing_profile_t
                     * unchanged by this pass, reused verbatim same as v9-v14's own
                     * comment; zones[] now typed zone_cfg_v16_t (see above) rather
                     * than the bare, now-stale `zone_cfg_t` name. */

/* Frozen v16 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 16->17): the single global ease_off_window_mult
 * scalar, appended at the true tail ahead of crc32, and zones[] still the
 * per-zone shape that predates this pass's per-zone override (zone_cfg_v16_t,
 * frozen above). This is what a LIVE, already-commissioned board looks like
 * on flash right now -- the exact blob a v16->v17 upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v16_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    float ease_off_window_mult; /* v16's global scalar -- removed at v17; see
                                 * zone_cfg_t::ease_off_window_mult's own
                                 * ZONES_CFG_VERSION 16->17 comment */
    uint32_t crc32;
} zones_cfg_v16_t; /* v16 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the per-zone ease_off_window_mult move and
                     * still carries the global scalar this pass removes. */

/* Frozen v17 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 17->18): zones[] is the per-zone shape that
 * predates this pass's approach_rate_cap_c_per_hr addition (zone_cfg_v17_t,
 * frozen above). Unlike v16, there is no wrapper-level scalar being removed
 * here -- ease_off_window_mult was already fully per-zone by v17, so this
 * wrapper's shape is otherwise identical to the CURRENT zones_cfg_t, just
 * with zones[] pinned to the smaller, historical per-zone type. This is what
 * a LIVE, already-commissioned v17 board looks like on flash right now --
 * the exact blob a v17->v18 upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v17_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v17_t; /* v17 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the per-zone approach_rate_cap_c_per_hr
                     * addition. */

/* Frozen v18 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 18->19): zones[] is the per-zone shape that
 * predates this pass's error_band_c/rate_band_c_per_s addition
 * (zone_cfg_v18_t, frozen above). Same shape as v17's own wrapper -- no
 * wrapper-level scalar removed here either, just zones[] pinned to the
 * smaller, historical per-zone type. This is what a LIVE, already-
 * commissioned v18 board looks like on flash right now -- the exact blob a
 * v18->v19 upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v18_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v18_t; /* v18 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the per-zone error_band_c/rate_band_c_per_s
                     * addition. */

/* Frozen v19 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 19->20): zones[] is the per-zone shape that
 * predates this pass's relay_type addition (zone_cfg_v19_t, frozen above).
 * Same shape as v18's own wrapper -- no wrapper-level scalar removed here
 * either, just zones[] pinned to the smaller, historical per-zone type. This
 * is what a LIVE, already-commissioned v19 board looks like on flash right
 * now -- the exact blob a v19->v20 upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v19_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v19_t; /* v19 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the per-zone relay_type addition. */

/* Frozen v20 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 20->21): zones[] is the per-zone shape that
 * predates this pass's settings_source[SRC_GROUP_COUNT] split
 * (zone_cfg_v20_t, frozen above). Same shape as v19's own wrapper -- no
 * wrapper-level scalar removed here either, just zones[] pinned to the
 * smaller, historical per-zone type. This is what a LIVE, already-
 * commissioned v20 board looks like on flash right now -- the exact blob a
 * v20->v21 upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v20_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v20_t; /* v20 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates the settings_source per-group split. */

/* Frozen v22 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 22->23): zones[] is the per-zone shape that
 * predates zone_type/failsafe_state/hyst_c/min_on_s/min_off_s
 * (zone_cfg_v22_t, frozen above). This is what a LIVE, already-commissioned
 * v22 board looks like on flash right now -- the exact blob a v22->v23
 * upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v22_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v22_t; /* v22 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates zone_type. */

/* Frozen v21 layout -- what zones_cfg_t looked like immediately before THIS
 * pass (ZONES_CFG_VERSION 21->22): zones[] is the per-zone shape that
 * predates progress_band_c (zone_cfg_v21_t, frozen above). This is what a
 * LIVE, already-commissioned v21 board looks like on flash right now -- the
 * exact blob a v21->v22 upgrade must read. */
typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v21_t zones[MAX31856_CHANNEL_COUNT];
    uint8_t timing_profile_count;
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v21_t; /* v21 -- what zones_cfg_t looked like immediately before THIS
                     * pass; predates progress_band_c. */



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

/* Bounded chain walk starting at `start`, following settings_source[group]
 * links through `zones[]` (MAX31856_CHANNEL_COUNT-sized, indexed exactly
 * like zones_cfg_t::zones). `group` selects which of the SRC_GROUP_COUNT
 * independent per-group bytes to walk (docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs -- each
 * group's chain is entirely independent of the others). Returns true if the
 * chain revisits a zone already on it -- a genuine inheritance cycle --
 * false if it terminates cleanly. See zones_config_json.c's own copy of
 * this function's original comment (moved verbatim) for the full walk
 * semantics. */
bool zones_config_json_settings_source_chain_has_cycle(const zone_cfg_t zones[MAX31856_CHANNEL_COUNT],
                                                        uint8_t group, uint8_t start, uint8_t thermo_count);

/* Every-load fixup, LOAD PATH ONLY: collapses any settings_source[group]
 * cycle in *cfg to ZONE_SETTINGS_SOURCE_CUSTOM on just the zones actually ON
 * the cycle, for EVERY group independently, logging each one by name
 * (`partition`, purely for the log line). A cycle can only reach flash via
 * firmware that predates the chain-walk guards, or direct NVS tampering;
 * either way a config that was valid before this guard shipped must keep
 * booting, not get wiped -- see zones_config_json.c's own copy of this
 * function's original comment for the full reasoning and the
 * index-order-independence argument. */
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

/* Runtime accessor pair for zone_cfg_t::ease_off_window_mult (ZONES_CFG_
 * VERSION 16->17, PER-ZONE as of this pass -- was a single global scalar at
 * 15->16, see that field's own comment for the move) -- declared here rather
 * than zones_http.h alongside this struct's other public getters/setters
 * purely because this pass's touched-files list does not include
 * zones_http.h; every caller that needs these (profile_executor_feedforward.c,
 * zones_http_get.c/zones_http_post.c, zones_config_accessors.c's own
 * definition) already includes or can include this header. Same "false means
 * cannot answer / reject" convention as every other zones_config_get_.../
 * set_...() pair in this codebase; `zone_index` is bounds-checked against
 * MAX31856_CHANNEL_COUNT the same way every other per-zone accessor in this
 * file is (e.g. zones_config_get_executor_thresholds()).
 *
 * zones_config_get_ease_off_window_mult() cannot fail on a normally-loaded
 * board for an in-range zone_index (the field is always a legal, currently-
 * in-effect value -- see the field's own doc comment) but still reports
 * success/failure like its siblings for a uniform call convention.
 * zones_config_set_ease_off_window_mult() rejects (false, no write, no NVS
 * save) a non-finite value or one outside [ZONE_EASE_OFF_WINDOW_MULT_MIN,
 * ZONE_EASE_OFF_WINDOW_MULT_MAX] -- same refuse-don't-clamp discipline
 * zones_config_set_coupling_diag_k_dc() documents, since silently clamping an
 * A/B campaign's requested value to a different one would corrupt the
 * experiment without telling anyone. */
bool zones_config_get_ease_off_window_mult(uint8_t zone_index, float *out_mult);
bool zones_config_set_ease_off_window_mult(uint8_t zone_index, float mult);

/* Runtime accessor pair for zone_cfg_t::approach_rate_cap_c_per_hr
 * (ZONES_CFG_VERSION 17->18, PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_
 * TARGET_DESIGN_STUDY.md option (b)) -- same declaration placement/rationale
 * as the ease_off_window_mult pair just above (this pass's touched-files
 * list does not include zones_http.h either). `zone_index` bounds-checked
 * against MAX31856_CHANNEL_COUNT, same as every other per-zone accessor.
 *
 * UNLIKE zones_config_get_ease_off_window_mult(), the getter here does NOT
 * resolve 0 into some other in-range value -- 0 IS the answer "uncapped,"
 * and profile_executor.c's caller must treat it that way (see the field's
 * own doc comment for why there is no sensible default cap to substitute).
 * A stored value that is finite but outside [MIN, MAX] and not exactly 0
 * (only reachable via direct NVS tampering or a rollback from newer
 * firmware with a wider range) is defensively treated as uncapped too --
 * same "never hand the caller a raw value it cannot safely act on" rule
 * zones_config_get_ease_off_window_mult() follows, just resolving to the
 * OTHER safe answer (off, not a substituted default) for this field.
 *
 * zones_config_set_approach_rate_cap_c_per_hr() rejects (false, no write, no
 * NVS save) a non-finite value or one outside {0.0f} union
 * [ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN, ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX]
 * -- refused, never clamped, same discipline as every setter in this file. */
bool zones_config_get_approach_rate_cap_c_per_hr(uint8_t zone_index, float *out_cap_c_per_hr);
bool zones_config_set_approach_rate_cap_c_per_hr(uint8_t zone_index, float cap_c_per_hr);

/* Runtime accessor pairs for zone_cfg_t::error_band_c / ::rate_band_c_per_s
 * (ZONES_CFG_VERSION 18->19, PID_EXPANSION_PLAN.md sec 3.6g) -- same
 * declaration placement/rationale as the ease_off_window_mult and
 * approach_rate_cap_c_per_hr pairs just above. `zone_index` bounds-checked
 * against MAX31856_CHANNEL_COUNT, same as every other per-zone accessor.
 *
 * Like zones_config_get_ease_off_window_mult() (and UNLIKE
 * zones_config_get_approach_rate_cap_c_per_hr()), 0 and anything outside
 * [MIN, MAX] resolve to the field's own firmware DEFAULT -- there is no
 * "no band" state a fuzzy membership function can mean the way "uncapped"
 * is a real state for a rate limiter, so every caller always gets back a
 * usable, finite, positive band width.
 *
 * zones_config_set_error_band_c()/zones_config_set_rate_band_c_per_s()
 * reject (false, no write, no NVS save) a non-finite value or one outside
 * {0.0f} union [MIN, MAX] for that field -- refused, never clamped, same
 * discipline as every setter in this file. */
bool zones_config_get_error_band_c(uint8_t zone_index, float *out_band_c);
bool zones_config_set_error_band_c(uint8_t zone_index, float band_c);
bool zones_config_get_rate_band_c_per_s(uint8_t zone_index, float *out_band_c_per_s);
bool zones_config_set_rate_band_c_per_s(uint8_t zone_index, float band_c_per_s);

/* Runtime accessor pair for zone_cfg_t::relay_type (ZONES_CFG_VERSION
 * 19->20, RELAY_LIFE_BUDGET.md) -- same declaration placement/
 * rationale as the pairs just above. `zone_index` bounds-checked against
 * MAX31856_CHANNEL_COUNT, same as every other per-zone accessor.
 *
 * zones_config_set_relay_type() rejects (false, no write, no NVS save) a
 * value outside [0, ZONE_RELAY_TYPE_MAX] -- refused, never clamped, same
 * discipline as every setter in this file. On success it also pushes the
 * new type to every relay named in this zone's relay_mask via
 * relay_cycles_set_type() (zones_config_store.c) -- the load path
 * (nvs_load_from()) makes the identical push directly rather than through
 * this setter, since load has no "successful save" to gate it, see that
 * call site's own comment. */
bool zones_config_get_relay_type(uint8_t zone_index, uint8_t *out_relay_type);
bool zones_config_set_relay_type(uint8_t zone_index, uint8_t relay_type);

/* Runtime accessor pair for zone_cfg_t::progress_band_c (ZONES_CFG_VERSION
 * 21->22, docs/audits/consumer_without_producer_2026-09-06.md finding 1) --
 * same declaration placement/rationale as the pairs above. `zone_index`
 * bounds-checked against MAX31856_CHANNEL_COUNT, same as every other
 * per-zone accessor.
 *
 * Like zones_config_get_error_band_c() (and UNLIKE
 * zones_config_get_approach_rate_cap_c_per_hr()), 0 and anything outside
 * [MIN, MAX] resolve to ZONE_PROGRESS_BAND_C_DEFAULT -- there is no "band
 * disabled" state guard 1's arrival test can accept, so every caller always
 * gets back a usable, finite, positive band width.
 *
 * zones_config_set_progress_band_c() rejects (false, no write, no NVS save)
 * a non-finite value or one outside {0.0f} union [ZONE_PROGRESS_BAND_C_MIN,
 * ZONE_PROGRESS_BAND_C_MAX] -- refused, never clamped, same discipline as
 * every setter in this file. */
bool zones_config_get_progress_band_c(uint8_t zone_index, float *out_band_c);
bool zones_config_set_progress_band_c(uint8_t zone_index, float band_c);

#ifdef __cplusplus
}
#endif

#endif // ZONES_CONFIG_JSON_H
