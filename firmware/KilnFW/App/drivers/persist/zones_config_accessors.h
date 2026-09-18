// zones_config_accessors -- persist-layer accessors for the zones config
// blob (TODO.md section 3), split out of zones_http.h (HW_ABSTRACTION.md
// "drivers/ layering" item 1, 2026-09-05): 18+ control/safety/persist files
// were including zones_http.h -- an HTTP-page module -- purely to reach these
// zones_config_*()/zones_current_sweep_*()/zones_ct_*() accessors and the
// bound constants (ZONE_*_MAX) they share with the POST /api/zones parser.
// This header holds that surface; zones_http.h now includes it and keeps
// only handler registration (zones_http_start()) and the hardware-wiring
// setter (zones_http_set_hw()) that main_network_http.c calls at boot.
// Definitions did not move -- they still live in zones_config_accessors.c,
// zones_http.c, zones_current_sweep_engine.c and zones_current_sweep_task.c,
// exactly as before this split.
//
// Scope: config storage and validation only. Historically one zone per
// configured thermocouple channel (zone i <-> channel i); TODO.md 10.8
// (2026-08-17, first slice) extends this to a many-to-one mapping via
// thermo_mask below -- a zone's control temperature can now be combined
// across more than one assigned channel. zones_config_get_thermo_mask()'s
// doc comment covers the legacy-mapping default this module falls back to
// when a caller (or an unmodified zones_page.html submission) never sends
// the field. cal_offset_c is stored
// here and applied through zones_config_apply_cal() below, which satisfies
// TODO.md section 3's "applied in firmware" half -- but only for the
// consumers that call it: dashboard_http.c's display and
// profile_executor.c's control math. uart_bridge.c (the PC/MCP UART path)
// still reports MAX31856_read()'s raw value, so those clients see an
// uncorrected number; see zones_config_apply_cal()'s scope note for why
// that gap is deliberate rather than overlooked.
//
// max_ramp_c_per_hr is a user-entered ceiling, not a PID-estimated one --
// TODO.md section 3 poses both options and explicitly leaves the
// PID-estimated one undesigned ("more work, needs a design of its own
// before it's buildable"). The user-entered ceiling is what
// zones_config_get_max_ramp() below hands to profiles_http.c's
// feasibility check (TODO.md section 5).
#ifndef ZONES_CONFIG_ACCESSORS_H
#define ZONES_CONFIG_ACCESSORS_H

#include <stdbool.h>
#include <stddef.h>

#include "zones_config_query.h"
#include <stdint.h>

#include "esp_err.h"
#include "MAX31856.h"
#include "heater_output.h" /* HEATER_MIN_ON_MS_FLOOR -- the relay-protection floor this
                             * file's ZONE_HEATER_MIN_ON_MS_FLOOR must not disagree with */
#include "kiln_io.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sanity bounds for the stored FOPDT plant model, shared by
 * zones_config_set_model() and any caller that must reject an out-of-range
 * model BEFORE writing anything (backup_http.c's import validation pass is
 * the reason these moved here, 2026-08-21: they used to be private #defines
 * in zones_http.c, so the import path could only check "finite and
 * non-negative" in its own first pass and had to let zones_config_set_model()
 * catch the real ceiling at commit time -- too late, since earlier entries in
 * the same import may already have been written to NVS by then. Now both
 * paths gate on the exact same two constants, so a whole-import validation
 * pass can refuse an out-of-range model before any write happens.
 *
 * These are typo/garbage filters, not physics: a kiln's static gain is order
 * 100-1000 degC per unit duty and its time constant order 1e3 s, so the
 * ceilings sit an order of magnitude clear of anything a real fit produces
 * while still rejecting a decimal-point slip. 86400 s (one day) is a hard "no
 * thermal process on this board is slower than this" bound. */
#define ZONE_MODEL_K_MAX 5000.0f
#define ZONE_MODEL_TIME_MAX_S 86400.0f

/* Sentinel for zone_cfg_t::model_fit_temp_c/model_fit_ambient_c (ZONES_CFG_
 * VERSION 23->24, docs/audits/high_temperature_transfer_analysis_2026-09-08.md):
 * "no operating point recorded for this fit". Deliberately NOT 0.0f -- 0 degC
 * is a plausible genuine ambient/operating temperature (a cold shop), so
 * using it as "unknown" would make a real measurement indistinguishable from
 * a missing one, exactly the conflation this sentinel exists to prevent.
 * -273.15 (absolute zero) is physically unreachable on a kiln, so no real
 * fit can ever collide with it. */
#define ZONE_MODEL_FIT_TEMP_UNKNOWN (-273.15f)

/* Every bound below moved out of zones_http.c (2026-08-21, backup-widening
 * pass) for the identical reason ZONE_MODEL_K_MAX/ZONE_MODEL_TIME_MAX_S moved
 * here first: backup_http.c's import validation pass must reject an
 * out-of-range value in pass 1, before any write happens, using the EXACT
 * same ceiling parse_zone_fields() (the POST /api/zones authority) and the
 * new zones_config_set_*() setters below enforce -- not a hand-copied mirror
 * that can silently drift out of sync with the real one. parse_zone_fields()
 * in zones_http.c was rewritten to reference these same macros instead of
 * its old inline literals. */

/* zone_cfg_t::name -- the operator-entered per-zone label. */
#define ZONE_NAME_MAX_LEN 15

/* relay_names_cfg_t::names[] -- the operator-entered label for a relay that
 * is NOT currently claimed by any zone (owner report, 2026-08-27+1: "the
 * user should be able to assign names to relays not assigned to zones as
 * well" -- the heating elements were just physically wired, and a coming
 * firing-profile feature lets a segment drive a bare relay directly, where
 * an operator picking "relay 3" off a dropdown should be picking "Vent
 * fan"). Same length as ZONE_NAME_MAX_LEN, deliberately NOT shortened the
 * way TIMING_PROFILE_NAME_MAX_LEN was: that cut was to buy back bytes inside
 * zones_cfg_t's 640-byte ZONES_CONFIG_BLOB_MAX_SIZE ceiling, and relay names
 * live in a SEPARATE NVS blob of their own (see zones_http.c's relay-names
 * section header comment) that never touches that ceiling at all -- there is
 * no budget pressure here motivating a shorter label than a zone gets. */
#define RELAY_NAME_MAX_LEN 15

/* relay_names_cfg_t::types[] -- what the operator says an extra relay
 * actually DRIVES (docs/ZONE_GRAPHIC_PLAN.md stage 1, RELAY_NAMES_CFG_VERSION
 * 1->2). A FIXED, code-defined enum, deliberately NOT operator-extensible:
 * the zone graphic draws one hand-drawn SVG icon per value, so a value with
 * no icon could not be rendered at all, and "let the operator invent a type"
 * would mean either a fallback glyph (the confident-lie failure mode that
 * plan's section 6 exists to prevent) or shipping a glyph editor.
 *
 * UNSET IS ENUM 0, AND THAT IS THE POINT (owner decision, 2026-09-18, closing
 * that plan's open question 1). Zero is what a migrated v1 blob, a
 * never-configured relay, and a memset()-zeroed struct all naturally land on,
 * so making zero mean `other` would have every board that upgrades silently
 * assert an operator choice nobody made. UNSET renders with the unknown glyph
 * -- "nobody has told me what this relay does" -- which is a different
 * statement from OTHER, a deliberate operator selection meaning "none of the
 * above". This is the same absent-is-not-zero, unknown-is-not-absent
 * distinction normal_current_measured and safety_wiring.tc_temp_valid already
 * carry elsewhere in this config.
 *
 * Values are ON FLASH (they are persisted in the relay-names blob), so an
 * existing value's number may never be reused for a different meaning --
 * append only, and bump RELAY_NAMES_CFG_VERSION if one is ever removed. */
typedef enum {
    RELAY_DEVICE_TYPE_UNSET = 0,
    RELAY_DEVICE_TYPE_DAMPER = 1,
    RELAY_DEVICE_TYPE_OUTLET = 2,
    RELAY_DEVICE_TYPE_VALVE = 3,
    RELAY_DEVICE_TYPE_FAN = 4,
    RELAY_DEVICE_TYPE_LIGHT = 5,
    RELAY_DEVICE_TYPE_OTHER = 6,
} relay_device_type_t;

/* One past the highest valid value -- the range check every setter and every
 * decode path uses. Derived from the enum rather than written as a literal so
 * appending a value cannot leave a hand-maintained bound behind (this repo's
 * single most repeated defect shape; see run_all_checks.ps1's own header on
 * hardcoded counts). */
#define RELAY_DEVICE_TYPE_COUNT ((uint8_t)(RELAY_DEVICE_TYPE_OTHER + 1))


/* zone_timing_profile_t::name -- the operator-entered label for a named
 * safety-timing profile (2026-08-27, ZONES_CFG_VERSION 8->9, owner's request:
 * "make it so that i can have diffrent Safety timings and assign the zones to
 * them ... that way i dont need to copy the data multipal times"). Deliberately
 * SHORTER than ZONE_NAME_MAX_LEN, not equal to it: zones_cfg_t now carries a
 * whole extra MAX31856_CHANNEL_COUNT-sized array (timing_profiles[]) that a
 * pre-timing-profiles board never had, and every byte this name field does not
 * use is a byte of headroom under ZONES_CONFIG_BLOB_MAX_SIZE (zones_http.c's
 * _Static_assert enforces the ceiling; see its own comment for the exact
 * budget). 7 is enough for "Default", "Bisque", "Glaze" and similar
 * short operator labels -- a profile name is chosen from a much smaller,
 * more repetitive vocabulary than a zone's free-form name. */
#define TIMING_PROFILE_NAME_MAX_LEN 7

/* SaftyFW's fixed count of current-sense channels (safety_link.h's
 * SAFETY_LINK_POWER_CHANNELS == 3, current_a[3] on the status frame) -- a
 * hardware fact about the safety processor, not an operator-configured count
 * like thermo_count/relay_count, so there is no "ct_count" field to keep
 * this in sync with. Lives here (not a private #define in zones_http.c)
 * because backup_http.c's import validator needs the exact same bound
 * zones_http.c's own ct_mask setter/POST-parser/import-validator enforce --
 * see zones_config_get_ct_mask()'s doc comment below. Bump if the hardware
 * ever grows a fourth CT channel. */
#define ZONE_CT_CHANNEL_COUNT 3u

/* zone_cfg_t::cal_offset_c -- degC. */
#define ZONE_CAL_OFFSET_MIN_C (-50.0f)
#define ZONE_CAL_OFFSET_MAX_C 50.0f

/* zone_cfg_t::max_ramp_c_per_hr -- 0 = never configured. */
#define ZONE_MAX_RAMP_C_PER_HR_MAX 1000.0f

/* zone_cfg_t::pid_kp/pid_ki/pid_kd -- no natural physical bound, so this is
 * generous headroom over anything this board's PID loop would ever be tuned
 * to, not a real per-field tuning limit. Named (not a bare literal) because
 * TWO call sites must enforce the exact same range: the whole-page POST
 * /api/zones parser (parse_zone_fields(), gated by ota_http_check_interlocks()
 * and refused outright while a firing is RUNNING/PAUSED) and the narrow POST
 * /api/zones/pid endpoint (zones_pid_post_handler(), the one write this file
 * allows while running -- see its own header comment for why that is safe).
 * zones_config_set_pid() -- the shared setter both paths that actually touch
 * storage end in -- enforces this same bound too, so a caller that reaches
 * it some other way (the LCD UI, PcTools/MCP) cannot exceed what either HTTP
 * path would accept. */
#define ZONE_PID_GAIN_MAX 1000.0f

/* zone_cfg_t::sanity_rate_c_per_min -- 0 = never configured (caller
 * substitutes its own default, does NOT disable the check). */
#define ZONE_SANITY_RATE_MAX_C_PER_MIN 20.0f

/* zone_cfg_t::max_temp_c/min_temp_c (guard 5) -- this IS the per-kiln safety
 * ceiling (thermal_guard.c guard 5, profile_executor_run.c's run-start
 * refusal), not an input-sanity bound. Owner request (2026-09-02, gas-kiln
 * follow-up): a legal ceiling must be settable above the hottest profile
 * anyone would author, and cone 42 (2015C) is the top of the standard
 * pyrometric cone table -- 2500C gives ~485C of headroom above that so the
 * ceiling can sit comfortably above the hottest realistic profile target
 * without being an absurd number to type into a form. profiles_http.c's
 * PROFILE_TARGET_C_MAX (a profile's own input-sanity bound, a DIFFERENT
 * question -- see that constant's own comment) is required to stay at or
 * below this one; profiles_http.c enforces that relationship with a
 * `_Static_assert` right next to its own definition, checked at compile
 * time rather than left to be kept equal by hand. float storage throughout
 * (zone_cfg_t::max_temp_c, thermal_guard_cfg_t::max_temp_c,
 * zones_config_json.h's per-version copies) -- no fixed-point encoding or
 * narrower type anywhere in the chain, so raising this bound cannot
 * truncate a legal high ceiling. No ZONES_CFG_VERSION bump needed: this is
 * a validation-time constant, not a change to what is stored or how. */
#define ZONE_MAX_TEMP_C_MAX 2500.0f
#define ZONE_MIN_TEMP_C_MIN (-50.0f)
#define ZONE_MIN_TEMP_C_MAX 200.0f

/* zone_cfg_t::autotune_baseline_k_dc (ZONES_CFG_VERSION 25->26, docs/audits/
 * adaptive_tune_vs_owner_requirements_2026-09-11.md) -- an ABSOLUTE ceiling
 * on a zone's static gain, unlike ZONE_MODEL_K_MAX just above (which that
 * constant's own comment already documents as "a typo/garbage filter, not
 * physics": 5000 is an order of magnitude past anything a real fit produces,
 * chosen only to catch a decimal-point slip). This one is derived from an
 * actual physical limit, not a margin-of-convenience: model_k_dc is defined
 * as the predicted steady-state temperature RISE above ambient at 100% duty
 * (zone_cfg_t::model_k_dc's own comment). ZONE_MAX_TEMP_C_MAX just above is
 * this same codebase's own considered answer to "what is the hottest
 * temperature this system is designed to ever legitimately report" --
 * 2500C, ~485C of headroom above cone 42, the top of the standard
 * pyrometric cone table. A fitted or blended K_dc predicting a steady-state
 * rise beyond that ceiling is not describing a real, reachable operating
 * point of THIS kiln; it is describing a temperature nothing in this
 * system's own safety envelope (guard 5, profile run-start refusal, every
 * MAX31856's own configured range) will ever let it actually reach or
 * report. Reusing ZONE_MAX_TEMP_C_MAX directly, rather than inventing a
 * second, separately-justified number, keeps this bound anchored to a
 * decision the codebase has already made about physical plausibility.
 *
 * This is the "absolute bound on K_dc, analogous to
 * ZONE_COUPLING_COEFF_MAX" adaptive_tune_model.c's blend/plausibility path
 * enforces before ever writing a new autotune_baseline_k_dc or blended
 * model_k_dc -- see ADAPTIVE_TUNE_K_DC_ABS_MAX (adaptive_tune_internal.h),
 * which is literally this constant, not a re-derivation of it, so the two
 * can never drift apart. */
#define ZONE_AUTOTUNE_K_DC_MAX ZONE_MAX_TEMP_C_MAX

/* zone_cfg_t::heater_window_ms/heater_min_on_ms/heater_min_off_ms -- 0 = not
 * configured (caller substitutes PROFILE_EXECUTOR_DEFAULT_*_MS). */
#define ZONE_HEATER_WINDOW_MS_MAX 600000.0f
#define ZONE_HEATER_MIN_ON_OFF_MS_MAX 60000.0f

/* zone_cfg_t::fuzzy_strength_pct (PID_EXPANSION_PLAN.md Phase 2/section
 * 3.3's "Adjustment strength" knob) -- 0-100, how far the fuzzy layer
 * (pid_fuzzy.c, a later pass) is allowed to move Kp/Ki/Kd away from the
 * base autotune-fitted gains. 0 = no adjustment at all, which is also the
 * safe default: a zone in ZONE_CONTROL_MODE_PID_FUZZY with strength 0 must
 * reproduce classic-PID output exactly (see PID_EXPANSION_PLAN.md Phase 6's
 * "strength_pct=0 reproduces the base gains bit-for-bit" negative test --
 * that is pid_fuzzy.c's obligation, not this file's, but the bound here is
 * what makes 0 reachable and typo'd values above 100% refused at the door). */
#define ZONE_FUZZY_STRENGTH_PCT_MAX 100.0f

/* zone_cfg_t::coupling_coeff[j] (PID_EXPANSION_PLAN.md section 2c's
 * cross-zone feedforward coefficients, one per neighbor since ZONES_CFG_
 * VERSION 10->11) -- each a measured, non-negative scalar in degC PER UNIT
 * DUTY AT ZONE j'S HEATER, the exact same convention as model_k_dc/ff_k_dc
 * (NOT a dimensionless ratio). zones[i].coupling_coeff[j] is ROW i, COLUMN
 * j: zone i's (the affected zone's) response to zone j's (the stepped
 * zone's) heater. Do not transpose -- the row is always the zone whose
 * feedforward is being computed.
 *
 * A plain `-coupling_coeff[j] * (T_j - T_j_setpoint)` is dimensionally
 * wrong -- it mixes degC_i/duty_j with degC_j and is NOT what shipped.
 * zone_feedforward() (profile_executor.c, Phase 3b) instead computes the
 * standard measured-disturbance feedforward, dividing by neighbor j's own
 * k_dc to first recover the dimensionless disturbance gain and then by this
 * zone's own k_dc to convert into this zone's duty:
 *
 *     -(coupling_coeff[j] / (k_dc_j * ff_k_dc_own)) * (T_j - T_j_setpoint)
 *
 * 0 = "no coupling measured against that neighbor", which is both the safe
 * default and the degrade-to-today behavior -- the plan's own words. This
 * ceiling is a typo/garbage filter like ZONE_MODEL_K_MAX, not a physics
 * bound: nothing about a kiln's radiative coupling has been measured yet to
 * derive a tighter one from.
 *
 * DELIBERATELY still non-negative, even after the 10->11 widening to a full
 * directed row: the feedforward's own leading minus sign already carries
 * the direction (a hotter-than-setpoint neighbor always pulls this zone's
 * feedforward down, a colder one pushes it up), so an independently-signed
 * coefficient would be redundant at best, a double-negative bug at worst.
 * Every coefficient measured on the bench so far is a positive cross-
 * heating gain; nothing today needs a negative (cooling) coupling to be
 * representable. Revisit this if that ever changes, but do not silently
 * relax it -- see zone_cfg_t::coupling_coeff's own doc comment in
 * zones_http.c for the full reasoning. */
#define ZONE_COUPLING_COEFF_MAX 100.0f

/* zone_cfg_t::settings_source (PID_EXPANSION_PLAN.md section 3.5's "Same as
 * zone N / Custom settings for this zone" UI dropdown) -- this exact
 * sentinel value means "this zone holds its own, custom settings", the only
 * legal value other than a real zone index < MAX31856_CHANNEL_COUNT. MUST be
 * the migration target for every zone in a v9->v10 upgrade (see
 * ZONES_CFG_VERSION's 9->10 comment in zones_http.c) -- migrating to 0
 * instead would silently mean "this zone copies zone 0's settings", which
 * Phase 5's (a later pass's) resolve-on-save logic would then overwrite
 * every zone with zone 0's numbers on the very next save. */
#define ZONE_SETTINGS_SOURCE_CUSTOM 0xFFu

/* ZONES_CFG_VERSION 20->21 (docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs): the single whole-zone
 * settings_source byte above became one byte PER MIRRORABLE GROUP -- the
 * owner wanted "same as zone N" per item, not all-or-nothing. Each group
 * below is independently either ZONE_SETTINGS_SOURCE_CUSTOM or a real zone
 * index, walked through the SAME chain-walk/cycle-collapse logic the old
 * single byte used (zone_settings_source_chain_has_cycle()), just once per
 * group instead of once per zone. Measured fields (model_*, coupling_*,
 * tuning_*, normals) and topology fields (name, relay_mask, thermo_mask,
 * ct_mask, relay_type) are NOT a group here and never mirror -- see
 * docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs's table. tc's cal_offset_c also never mirrors
 * (per-sensor); only tc_type does, hence "tc" rather than "thermocouple". */
#define SRC_GROUP_LIMITS 0        /* max_temp_c, min_temp_c, max_ramp_c_per_hr, sanity_rate_c_per_min */
#define SRC_GROUP_RELAY_TIMING 1  /* heater_window_ms, heater_min_on_ms, heater_min_off_ms */
#define SRC_GROUP_CONTROL 2       /* control_mode, pid_kp/ki/kd, fuzzy_strength_pct */
#define SRC_GROUP_GUARDS 3        /* the eight guard_* thresholds, cross_zone_max_delta_c */
#define SRC_GROUP_TC 4            /* tc_type ONLY -- cal_offset_c stays per sensor, never mirrors */
#define SRC_GROUP_COUNT 5

/* heater_min_on_ms is the ONE heater timing field with a lower bound as well
 * as an upper one, and the bound is a hardware-protection minimum rather than
 * a sanity ceiling: HEATER_MIN_ON_MS_FLOOR (heater_output.h, 10 s, set by the
 * owner 2026-08-28) is how long the PID relay must stay closed once it has
 * been commanded on, so the downstream contactor is not cycled to death.
 *
 * Accepted values are therefore DISJOINT, not a range: 0 ("not configured" --
 * the caller substitutes HEATER_DEFAULT_MIN_ON_MS, which IS the floor), or
 * anything from the floor up to ZONE_HEATER_MIN_ON_OFF_MS_MAX. A submission
 * strictly between the two is REFUSED, with an error naming the floor, rather
 * than silently stored.
 *
 * Refusing rather than clamping is a deliberate choice for this field, and it
 * is the opposite of what the rest of this file does with an out-of-range
 * number (every other field is bounded by parse_float_field() and a
 * submission outside its bounds is likewise refused -- so this is actually
 * CONSISTENT with them; nothing in zones_http.c silently clamps). The reason
 * it matters more here: heater_output_duty() would go on running such a zone
 * perfectly safely, at 10 s, while /api/zones read back 3000 -- an operator
 * would be looking at a number the kiln is not using. The refusal is the only
 * way the web UI can be honest about it.
 *
 * heater_output_duty()'s own floor stays regardless, as defense in depth:
 * this check guards the HTTP door, not the only door (backup import, an older
 * NVS blob written before the floor existed, a future caller of
 * zones_config_set_heater_cfg()). Stored blobs written before 2026-08-28 are
 * raised to the floor on load -- see raise_heater_timing_to_floors() in zones_http.c
 * -- so a GET never reports a sub-floor value that a POST would then bounce. */
#define ZONE_HEATER_MIN_ON_MS_FLOOR ((float)HEATER_MIN_ON_MS_FLOOR)

/* heater_window_ms's lower bound, and the second of this file's two
 * disjoint-set heater rules (2026-08-29). Accepted values are 0 ("not
 * configured" -- the caller substitutes HEATER_DEFAULT_WINDOW_MS, 60 s,
 * which satisfies the rule) or anything from
 *
 *     ZONE_HEATER_WINDOW_MIN_MULTIPLE * max(heater_min_on_ms, floor)
 *
 * up to ZONE_HEATER_WINDOW_MS_MAX. Unlike every other bound in this file
 * this one is not a constant: it depends on the SAME zone's
 * heater_min_on_ms, because what it protects is the relationship between
 * the two, not either number on its own.
 *
 * Why it exists: a window shorter than the min-on floor cannot express any
 * fractional duty at all -- see HEATER_MIN_WINDOW_MULTIPLE in
 * heater_output.h for the zone-0 failure that produced this rule. Both
 * numbers were individually inside their own ranges and individually
 * sensible; only their ratio was wrong, which is exactly the class of
 * mistake a per-field range check cannot catch.
 *
 * Enforced the same four places heater_min_on_ms's floor is: refused by
 * parse_zone_fields() (HTTP), refused by zones_config_set_heater_cfg()
 * (backup import and any other non-HTTP door), refused by
 * validate_zones_cfg(), and RAISED on load by raise_heater_timing_to_floors()
 * so a board configured before this rule existed still round-trips a GET
 * into a POST. heater_output_cfg_expressible() is the point-of-use check
 * behind all of them. */
#define ZONE_HEATER_WINDOW_MIN_MULTIPLE ((float)HEATER_MIN_WINDOW_MULTIPLE)

/* The smallest heater_window_ms acceptable for a zone whose heater_min_on_ms
 * is min_on_ms (0 = not configured, i.e. the floor). Mirrors
 * heater_output_required_window_ms() in float. */
static inline float zone_required_window_ms(float min_on_ms)
{
    float effective = (min_on_ms > ZONE_HEATER_MIN_ON_MS_FLOOR) ? min_on_ms : ZONE_HEATER_MIN_ON_MS_FLOOR;
    return effective * ZONE_HEATER_WINDOW_MIN_MULTIPLE;
}

/* zone_cfg_t's 8 named guard-threshold overrides (TODO.md 6A.3) -- 0
 * substitutes thermal_guard.c's own firmware default for every one of these,
 * it does NOT disable the guard (opposite convention to cross_zone_max_delta_c
 * below). Bounds are sanity ceilings against a typo'd submission, matching
 * parse_zone_fields()'s own comment. */
#define ZONE_GUARD_TIME_S_MAX 7200.0f
#define ZONE_GUARD_RATE_C_PER_MIN_MAX 20.0f
#define ZONE_GUARD_MARGIN_C_MAX 500.0f
#define ZONE_GUARD_DEBOUNCE_TICKS_MAX 100.0f

/* zone_cfg_t::cross_zone_max_delta_c (guard 8) -- 0 = not configured, which
 * DISABLES the guard rather than substituting a default (see the getter's
 * own doc comment for why this one field's convention is the opposite of the
 * guard thresholds above). */
#define ZONE_CROSS_ZONE_DELTA_C_MAX 1000.0f

/* The nine v8 thermal-timing overrides plus the one global
 * (pc_link_abort_silence_ms), added 2026-08-27 at the owner's request that the
 * remaining thermal-protection constants stop being magic numbers. As of
 * ZONES_CFG_VERSION 9 (same day, follow-up owner request: "make it so that i
 * can have diffrent Safety timings and assign the zones to them ... that way i
 * dont need to copy the data multipal times") the nine live on
 * zone_timing_profile_t -- a NAMED profile a zone points at via
 * zone_cfg_t::timing_profile -- rather than being duplicated on every
 * zone_cfg_t; pc_link_abort_silence_ms stays a single top-level field, since
 * it describes one PC link, not something a zone experiences. Same convention
 * as the guard thresholds above: 0 = not configured, the module substitutes
 * its own named firmware default; 0 never disables. Bounds are sanity
 * ceilings against a typo'd submission.
 *
 * The duty bound is 1.0 because it is a duty fraction, not a percentage --
 * guard 1 arms when commanded duty is at or above it, so a value above 1.0
 * would arm the guard never and silently switch off the protection. That is
 * exactly the kind of "configured it into uselessness" the ceiling exists to
 * refuse. */
#define ZONE_GUARD_DUTY_MAX 1.0f
#define ZONE_GUARD_EPS_C_MAX 10.0f
#define ZONE_PC_LINK_SILENCE_MS_MAX 600000.0f

/* Read-only accessors for profiles_http.c's feasibility check (TODO.md
 * section 5) -- it must not touch NVS or this module's storage directly,
 * only ask these two questions. */

/* zone_index is 0-based, must be < the configured thermo_count. Returns
 * false (and leaves *out_c_per_hr untouched) for an out-of-range or
 * unconfigured zone -- the caller is expected to treat that as "cannot
 * check feasibility," not as a ceiling of 0. */
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr);

/* zones_config_get_thermo_count() moved to zones_config_query.h (included
 * above, HW_ABSTRACTION.md item 11, 2026-09-05) -- it is the one
 * accessor a caller can need without the rest of this persist-tier surface
 * (sim_backend.c and, transitively, everything below still reach it). */

/* How many of the KILN_IO_RELAY_COUNT relays are currently configured on
 * this board -- the same bound parse_zone_fields()'s z%u_relay_mask
 * validation and zones_config_set_relay_mask() apply to a submitted
 * relay_mask (a bit past this count references a relay that doesn't exist).
 * backup_http.c's import validation pass needs this to reject an
 * out-of-range relay_mask in pass 1, before any write happens -- the same
 * reason zones_config_get_thermo_count() already existed for thermo_mask/
 * zone_mask checks. */
uint8_t zones_config_get_relay_count(void);

/* TODO.md 6A.5 load-staggering: board-wide cap on relays energized at once
 * (0 = unlimited, the default). Global, not per-zone -- enforced by
 * profile_executor.c against every active zone's commanded relay state in
 * a run, regardless of which zones it spans. */
uint8_t zones_config_get_max_simultaneous_relays(void);

/* TODO.md 6A.3's "default policy on a single-zone trip: abort the whole
 * firing" -- false (the default, matching a migrated pre-2026-08-13 blob
 * that predates this field) means abort; true is the explicit opt-in to
 * continue with the other healthy zones. Global, not per-zone -- see
 * zones_cfg_t::continue_on_zone_trip's comment. */
bool zones_config_get_continue_on_zone_trip(void);

/* TODO.md owner-report item 3 (2026-08-21): the RP2040 safety processor's
 * OWN, physically independent thermocouple type, as last saved on the
 * Thermocouples & Zones page. Consumed by safety_link.c to mirror this
 * setting to the Pico over SAFETY_CMD_SET_CONFIG and to re-apply it if the
 * link was down when it changed -- see safety_link.c's safety_sync_tc_type()
 * for the full mirroring design and why this is a SEPARATE setting from any
 * main-board zone's own tc_type.
 *
 * Returns false only for a NULL out pointer -- this is a global setting with
 * a real value from the moment NVS loads, so there is no "cannot answer,
 * zone not configured" case the way there is for the per-zone getters below.
 * Callers that care whether the returned value came from a genuinely loaded
 * config (vs. the zeroed default of a board that has never saved one) must
 * check zones_config_is_valid() themselves -- see this getter's own comment
 * in zones_http.c for why 0 is not a safe "unconfigured" reading here. */
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type);

/* 2026-08-21, LCD item 1: getter/setter pair for one channel's own
 * thermocouple type -- see zones_config_get_pid()'s/zones_config_set_pid()'s
 * doc comments for the shared "false means cannot answer"/generation-bump
 * conventions this follows exactly. zone_index is 0-based, must be < the
 * configured thermo_count (same bound zones_config_get_max_ramp() uses).
 * zones_config_set_tc_type() rejects tc_type > 7 (ZONE_TC_TYPE_MAX_REAL in
 * zones_http.c) -- a voltage-input mode is never legal from an
 * operator-facing type selector, LCD or web. See zones_http.c's own comment
 * on this pair for why the setter deliberately does NOT push the new value
 * to the live MAX31856 register (the web POST path doesn't either; both take
 * effect on the next boot only, today). */
bool zones_config_get_tc_type(uint8_t zone_index, uint8_t *out_tc_type);
bool zones_config_set_tc_type(uint8_t zone_index, uint8_t tc_type);

/* Setter for zones_config_get_safety_tc_type()'s global RP2040-safety-
 * processor setting. Same bound/shape as zones_config_set_tc_type() above. */
bool zones_config_set_safety_tc_type(uint8_t tc_type);

/* TODO.md 8.2 "Tie it to the guards, not only the UI". true only after a
 * real, trustworthy zones config is live -- a successful load (current
 * version, or an older version successfully migrated) or a fresh validated
 * POST /api/zones save. false whenever the loader hit the version-refuses
 * (newer-than-firmware) path, a corrupt/wrong-size blob, an NVS partition
 * that failed to come up, or the namespace was simply never created
 * (first boot). Distinct from "thermo_count == 0", which is a legitimately
 * saved empty config, not a failed load.
 *
 * profile_executor.c and autotune_engine.c must refuse to start a run when
 * this is false, with an explicit reason -- not rely on relay_mask reading
 * as 0 by coincidence of the zeroed struct. dashboard_http.c reports this
 * on /api/status as "zones_config_valid" so the on-device dashboard and the
 * pc_tools GUI's Zones panel can say so instead of silently doing nothing. */
bool zones_config_is_valid(void);

/* CLAUDE.md's ota_rollback_esp() hazard, closed 2026-09-16: naming WHY the
 * config failed to load, distinct from "never configured" (a legitimate
 * first-boot state also covered by zones_config_is_valid()==false above).
 * Latched once, for the boot, the first time nvs_load() actually finds a
 * real prior blob in KILN_NVS_PARTITION that this firmware could not turn
 * into a trustworthy zones_cfg_t -- either because the blob's own version is
 * NEWER than ZONES_CFG_VERSION (a rollback past a schema bump, or newer
 * firmware than this build), or because it is OLDER than this firmware can
 * migrate forward (docs/CONFIG_MIGRATION_CHAIN_PLAN.md's one-step-at-a-time
 * policy: a future build carrying only the N-1->N step cannot consume a
 * blob more than one step behind). Never cleared by a later in-RAM event --
 * like recovery_mode, this is fixed for the boot; only a fresh boot (after a
 * reflash or a fresh save that lands a new, valid blob) re-evaluates it. A
 * board with NOTHING ever saved (fresh out of the box) leaves this false: no
 * prior data existed to fail decoding, so there is nothing to warn about. */
typedef enum {
    ZONES_CFG_LOAD_FAULT_NONE = 0,    /* no fault latched this boot */
    ZONES_CFG_LOAD_FAULT_NEWER,       /* on-disk version > ZONES_CFG_VERSION */
    ZONES_CFG_LOAD_FAULT_UNREADABLE,  /* on-disk version this fw cannot decode/migrate (too old, corrupt, bad CRC/length) */
} zones_cfg_load_fault_kind_t;

typedef struct {
    bool occurred;
    zones_cfg_load_fault_kind_t kind;
    uint8_t on_disk_version;   /* the blob's own claimed version byte */
    uint8_t fw_version;        /* ZONES_CFG_VERSION at the moment this was captured */
    char reason[96];           /* zones_config_json_decode_blob()'s own *err_reason, copied */
} zones_cfg_load_fault_t;

/* Returns the latched fault (see the type's own comment above). Always
 * answerable -- when nothing was ever latched this boot, *out (if given) is
 * zeroed with occurred==false and the return value is false. Cheap RAM read,
 * safe from any task including the LVGL/httpd stacks. */
bool zones_config_get_load_fault(zones_cfg_load_fault_t *out);

/* M13 ("every fault says what was detected and what to do", ROADMAP.md,
 * standing rule): the single-migration-step hazard fix (2026-09-16,
 * zones_config_persist_migrated_blob_verified() in zones_config_store.c)
 * writes-back-and-verifies a just-migrated blob so a later, one-step-only
 * firmware can still consume it. When that write-back itself cannot be
 * confirmed (write failed, or a read-back mismatch after the bounded
 * retry), this boot keeps running fine on the in-RAM migrated copy, but
 * flash still holds the OLDER bytes -- the very next firmware install one
 * step further will find them unreadable and fall back to defaults with no
 * separate warning, same hazard shape as zones_cfg_load_fault_t above. That
 * used to be logged only via ESP_LOGE, invisible to the operator. Latched
 * once, for the boot, the first (and only) time
 * zones_config_persist_migrated_blob_verified() gives up; never cleared
 * mid-boot. A board that never attempted a migration this boot, or whose
 * write-back verified fine, leaves this false. */
typedef struct {
    bool occurred;
    uint8_t on_disk_version;  /* the pre-migration on-disk version byte */
    uint8_t fw_version;       /* ZONES_CFG_VERSION at the moment this was captured */
} zones_cfg_migration_persist_fault_t;

/* Returns the latched migration write-back fault (see the type's own
 * comment above). *out (if given) is zeroed with occurred==false when
 * nothing was latched this boot. Cheap RAM read. */
bool zones_config_get_migration_persist_fault(zones_cfg_migration_persist_fault_t *out);

/* Below: read-only accessors for profile_executor.c (TODO.md section 6).
 * Same rule as zones_config_get_max_ramp -- false means "cannot answer,"
 * not "answer is zero." */

/* bit N-1 = relay N belongs to this zone (matches zone_cfg_t::relay_mask,
 * the schematic's Relay1..4 numbering per kiln_io.h). */
bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask);

/* Setter for zones_config_get_relay_mask() above -- same "false means cannot
 * answer/rejected, nothing written" shape as every setter in this file.
 * relay_mask is checked against the CURRENTLY configured relay_count, the
 * same bound parse_zone_fields()'s z%u_relay_mask handling enforces (a bit
 * past relay_count references a relay that does not exist on this board). */
bool zones_config_set_relay_mask(uint8_t zone_index, uint8_t relay_mask);

/* TODO.md 10.8: bit N-1 = MAX31856 channel N belongs to this zone's control
 * temperature, N in 1..MAX31856_CHANNEL_COUNT -- same bit-numbering
 * convention as relay_mask above, deliberately: it is the natural pattern
 * this codebase already established for "which of a fixed hardware set
 * belongs to this zone," and following it means a caller that already
 * groks relay_mask reads this correctly on sight.
 *
 * Unlike relay_mask, a saved 0 here is NOT "nothing assigned" for a zone
 * that predates this field or whose submitter never heard of it -- see
 * zones_http.c's POST handler and migrate_zones_cfg_v1_to_current(). Both
 * substitute the legacy single-channel mapping (bit (zone_index) set, i.e.
 * "zone i reads channel i", matching this module's pre-10.8 behaviour)
 * whenever the field was never explicitly supplied, so an existing saved
 * config or an unmodified zones_page.html submission keeps controlling off
 * the same channel it always did. An explicit POST of 0 (a client that DOES
 * know this field and deliberately clears it) is honoured as a real "no
 * thermocouple assigned" and reaches this getter as 0 -- profile_executor.c
 * then sees the same "thermocouple invalid" state a single unassigned
 * channel has always produced (TODO.md 10.8: "zero valid readings ... is
 * the existing thermocouple invalid case, unchanged").
 *
 * Returns false (leaving *out_mask untouched) for the same "cannot answer"
 * reasons every getter here does -- an out-of-range or unconfigured
 * zone_index. */
bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask);

/* Setter for zones_config_get_thermo_mask() above. Unlike the POST
 * /api/zones handler's z%u_thermo_mask handling, this setter has no
 * "omitted means preserve the legacy zone-i<->channel-i mapping" case --
 * there is no such thing as "omitted" for a single explicit setter call, only
 * a value the caller passes or doesn't call this at all. A caller (backup
 * import) that wants "leave this zone's thermo_mask alone" simply does not
 * call this setter for that zone, same as it does for every other optional
 * field. thermo_mask is checked against the CURRENTLY configured
 * thermo_count, same bound parse_zone_fields() enforces. */
bool zones_config_set_thermo_mask(uint8_t zone_index, uint8_t thermo_mask);

/* 2026-08-27: bit N-1 = SaftyFW current-sense channel N feeds this zone's
 * live-current display, N in 1..ZONE_CT_CHANNEL_COUNT (a fixed hardware
 * count, unlike relay_mask/thermo_mask which are bounded by the operator-
 * configured relay_count/thermo_count) -- see zones_http.c's ZONES_CFG_VERSION
 * 5->6 comment. Purely informational: nothing in the control/guard path reads
 * this, only the settings page's own readout. Unlike thermo_mask, 0 is
 * unconditionally "no probe mapped" with no legacy fallback -- there is no
 * pre-existing single-CT-per-zone convention to preserve. */
bool zones_config_get_ct_mask(uint8_t zone_index, uint8_t *out_mask);

/* Setter for zones_config_get_ct_mask() above -- same shape as
 * zones_config_set_thermo_mask(). ct_mask is checked against the fixed
 * ZONE_CT_CHANNEL_COUNT, not against any operator-configured count. */
bool zones_config_set_ct_mask(uint8_t zone_index, uint8_t ct_mask);

/* TODO.md 10.3: the operator-chosen name a zone_cfg_t already stores
 * (ZONE_NAME_MAX_LEN, currently 15 chars) but which, until now, had no
 * public accessor -- zones_http.c's own JSON responses read it straight off
 * the struct, and no other consumer had asked for it. ui_page_home.c is
 * that second caller, needing something better than "Zone N" for its
 * per-zone cards, hence this getter now existing (same "extract when the
 * second caller shows up" discipline as every other getter in this file).
 *
 * Copies at most out_cap-1 bytes plus a NUL terminator into *out. Returns
 * false (leaving *out untouched) for an out-of-range or unconfigured
 * zone_index, or a NULL/zero-capacity out buffer -- same "cannot answer"
 * convention as every getter above. A TRUE return with an empty string is
 * a real, different case: a configured zone that has never been given a
 * name. Callers must treat that the same as "no name" (e.g. fall back to a
 * generated "Zone N" label), not as a getter failure. */
bool zones_config_get_name(uint8_t zone_index, char *out, size_t out_cap);

/* Setter for the getter above. Rejects (without writing anything) a name
 * longer than ZONE_NAME_MAX_LEN -- the same "zone name too long" rejection
 * parse_zone_fields() produces when http_form_find_field() returns -2 for
 * z%u_name, applied here to a NUL-terminated C string instead of a form
 * field. name may be NULL, treated the same as an empty string (clears the
 * zone's name), matching a POST that omits z%u_name. */
bool zones_config_set_name(uint8_t zone_index, const char *name);

/* Owner report 2026-08-27+1 ("the user should be able to assign names to
 * relays not assigned to zones as well"): the operator-entered name for one
 * physical relay, stored INDEPENDENTLY of zone assignment -- see
 * zones_http.c's relay-names section header comment for why this lives in
 * its own NVS blob rather than as a field on zone_cfg_t/zones_cfg_t, and for
 * the "name survives its relay becoming zone-owned" decision.
 *
 * relay_n is 1-based (kiln_io.h's Relay1..KILN_IO_RELAY_COUNT numbering,
 * matching zones_config_get_relay_mask()'s bit convention -- bit N-1 there
 * is "relay N" here). Returns false (leaving *out untouched) for an
 * out-of-range relay_n or a NULL/zero-capacity out buffer, same "cannot
 * answer" convention as zones_config_get_name(). A TRUE return with an
 * empty string is a real, different case: a relay that has never been
 * named. Deliberately does NOT report whether relay_n is currently
 * zone-owned -- that is a live computation over the CURRENT zone config
 * (zone assignment can change at any time), which callers make themselves
 * the same way rules_task.c's compute_heater_relay_mask() / rules_http.c's
 * check_relay_not_zone_owned() already do, not something baked into this
 * getter's answer. */
bool zones_config_get_relay_name(uint8_t relay_n, char *out, size_t out_cap);

/* Setter for the getter above. Rejects (without writing anything) a name
 * longer than RELAY_NAME_MAX_LEN. Does NOT check zone ownership -- storage
 * is unconditional; see this pair's header comment and zones_http.c's
 * relay-names section for why a name is kept, not cleared, when its relay
 * becomes zone-owned. name may be NULL, treated as an empty string (clears
 * the name), matching zones_config_set_name()'s own convention. */
bool zones_config_set_relay_name(uint8_t relay_n, const char *name);

/* The device-type companion to the relay-name pair above
 * (docs/ZONE_GRAPHIC_PLAN.md stage 1): what this relay actually drives.
 * relay_n is 1-based, same convention as the name pair.
 *
 * The getter returns false (leaving *out untouched) only for an out-of-range
 * relay_n or a NULL out -- "cannot answer". A TRUE return carrying
 * RELAY_DEVICE_TYPE_UNSET is a real, different case and callers must keep it
 * distinct: it means the relay exists and nobody has said what it drives,
 * NOT that it drives nothing and NOT that it is RELAY_DEVICE_TYPE_OTHER.
 * Rendering UNSET as anything other than the unknown glyph is the exact
 * regression that plan's section 6 forbids.
 *
 * The setter rejects (writing nothing) an out-of-range relay_n or a type at
 * or past RELAY_DEVICE_TYPE_COUNT -- an unknown type number is never stored
 * and never guessed at, the same refuse-rather-than-guess discipline
 * zones_config_json_decode_blob() applies to a newer-than-known version. */
bool zones_config_get_relay_device_type(uint8_t relay_n, relay_device_type_t *out);
bool zones_config_set_relay_device_type(uint8_t relay_n, relay_device_type_t type);


/* PID_EXPANSION_PLAN.md 3.3, "consolidate the opt-in flag into the zone
 * config blob": adaptive_tune.c's per-zone continuous-tuning opt-in,
 * consolidated here (ZONES_CFG_VERSION 13->14) out of that module's own
 * former 'adap_tune' NVS namespace. Same "false means off (or out-of-range,
 * same answer either way)" convention as every other simple per-zone flag
 * in this file -- there is no separate "cannot answer" case, unlike the
 * model_ and tuning_ getters above: opt-in is always meaningful, and its documented default
 * is OFF, so an out-of-range zone reading as "not enabled" is exactly
 * right, not a getter failure being mistaken for a real answer. */
bool zones_config_get_adaptive_tune_enabled(uint8_t zone_index);

/* Setter for the getter above. Persists immediately (nvs_save()), same
 * discipline as every setter in this file -- see zones_config_set_pid()'s
 * own header comment for the "own the write" convention this follows. */
bool zones_config_set_adaptive_tune_enabled(uint8_t zone_index, bool enabled);

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd);

/* Writer for pid_autotune's results-acceptance flow (TODO.md 6A.4:
 * "results are proposed, never auto-applied... only then does it get
 * written through zones_http's config, which stays the one owner of zone
 * config -- autotune must not write NVS itself"). Only touches kp/ki/kd,
 * persists immediately, and returns false without writing anything for an
 * out-of-range zone_index or a non-finite/negative gain -- same validation
 * discipline as the form-submit path, just for a single field group instead
 * of the whole page. */
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd);

/* The FOPDT plant model autotune fitted for this zone (TODO.md 6A.4),
 * persisted so TODO.md 6A.2's feedforward term
 * u_ff = (T_sp - T_ambient)/K_dc + (dT_sp/dt)*tau/K_dc has something to
 * stand on after a reboot. Before this the fit lived only in
 * autotune_engine.c's RAM for the duration of one run, which is no place
 * for a number that costs the operator hours of real kiln heat to measure.
 *
 * Same "false means cannot answer, not answer is zero" convention as every
 * getter above -- and here the caller must apply it to the VALUES too: 0 in
 * any of the three outputs means no model has been identified for this
 * zone, and a caller that treats that as a real model divides by zero in
 * the expression above. "No model" is the expected state of a zone that has
 * never been autotuned, so the feedforward term is simply off for it (which
 * is what 6A.2 specifies: "default on once autotune has run, off before
 * that"), not an error to report. */
bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s);

/* Writer for the same results-acceptance flow zones_config_set_pid() serves,
 * and subject to the same 6A.4 rule: autotune proposes, the operator
 * accepts, and only then does zones_http -- the one owner of zone config --
 * write it. Only touches the three model fields, persists immediately, and
 * returns false without writing anything for an out-of-range zone_index or
 * a non-finite/negative/absurd parameter, so a rejected call can never
 * leave a zone holding K from one run and tau from another.
 *
 * Writing all zeros is legal and is how a caller clears a stale model
 * (e.g. after the kiln's load or element set changed enough that the old
 * fit is a lie); it is not treated as a validation failure. */
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s);

/* model_fit_temp_c/model_fit_ambient_c getter/setter (ZONES_CFG_VERSION
 * 23->24) -- the operating point a model was fitted at, deliberately kept
 * separate from zones_config_get/set_model() itself rather than widening
 * that function's signature: every existing caller of set_model() (the
 * adaptive-tune blend path, the UART bridge, backup restore) writes a K/tau/
 * dead-time triple without necessarily having a fresh measured temperature
 * in hand, and forcing all of them to invent one would be exactly the
 * "invented sentinel" this pass exists to avoid. Callers that DO have a real
 * measurement (autotune_engine_guard.c's accept path, immediately after its
 * own zones_config_set_model() call) call this separately, with the actual
 * baseline/ambient the step test measured -- never the setpoint.
 *
 * Out-of-range zone_index, or a non-finite/wildly-implausible temperature
 * that is not the ZONE_MODEL_FIT_TEMP_UNKNOWN sentinel, is refused without
 * writing anything, same reject-nothing-half-applied discipline as
 * zones_config_set_model(). Passing the sentinel for both is how a caller
 * explicitly marks a fit's context as unknown/cleared. */
bool zones_config_get_model_fit_context(uint8_t zone_index, float *out_fit_temp_c, float *out_fit_ambient_c);
bool zones_config_set_model_fit_context(uint8_t zone_index, float fit_temp_c, float fit_ambient_c);

/* zone_cfg_t::autotune_baseline_k_dc (ZONES_CFG_VERSION 25->26) -- the K_dc
 * value the last full autotune Accept actually wrote, kept separate from
 * model_k_dc itself for the identical reason model_fit_temp_c/
 * model_fit_ambient_c are kept separate from it: adaptive_tune_model.c's
 * refine path needs an anchor that does NOT move every time IT writes
 * model_k_dc, or the anchor and the thing it bounds become the same moving
 * target (see docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md
 * and this field's own zone_cfg_t comment for the full "bound relative to
 * persisted state ratchets" defect this exists to close).
 *
 * 0 = "no baseline recorded yet" -- same sentinel convention model_k_dc
 * itself uses for "no model". Getter returns false only for an out-of-range
 * zone_index (same as every getter in this file); the 0 sentinel is a valid,
 * meaningful answer, not a "cannot answer". Setter refuses (without writing)
 * a non-finite, negative, or > ZONE_AUTOTUNE_K_DC_MAX value -- see that
 * constant's own comment for the physical justification. Callers: (1)
 * autotune_engine_guard.c's accept path, immediately after a fresh
 * zones_config_set_model() lands from a NEW full autotune run (re-anchors to
 * the new measurement); (2) adaptive_tune_refine_zone_locked() itself, the
 * first time it runs against a zone whose model predates this field
 * (bootstraps the anchor from the live model_k_dc rather than refusing
 * outright, so an already-autotuned board is not stuck at "no baseline"
 * forever just because it upgraded before this field existed). */
bool zones_config_get_autotune_baseline_k_dc(uint8_t zone_index, float *out_k_dc);
bool zones_config_set_autotune_baseline_k_dc(uint8_t zone_index, float k_dc);

/* zone_model_at()/coupling_at() -- HIGH_TEMPERATURE_TRANSFER_ANALYSIS's
 * "cheap seam" (item 2): every control-path reader of a zone's plant model
 * or coupling row goes through here instead of calling zones_config_get_
 * model()/zones_config_get_coupling() directly. Today T_c is accepted and
 * ignored -- these are exact passthroughs to today's constant reads, bit-
 * identical to calling the underlying getter directly -- so a later
 * temperature-dependent implementation (the gain schedule the analysis
 * argues ki, not Kp, will need) touches this one function instead of every
 * call site that reads a model or coupling row. Deliberately NOT
 * implemented as a schedule this pass -- see that doc's own conclusion:
 * scheduling now, on the single unrecorded operating point every existing
 * fit was taken at, would be fitting noise. */
bool zone_model_at(uint8_t zone_index, float T_c, float *out_k_dc, float *out_tau_s, float *out_dead_time_s);
bool coupling_at(uint8_t zone_index, float T_c, float out_row[MAX31856_CHANNEL_COUNT]);

/* ZONES_CFG_VERSION 12->13 (2026-09-01, owner: "in the pid stistics
 * consider, maybe there should be 2 sets, one for the pid tuneing that
 * stays unless retuned, and another for the last fireing run"). This is set
 * 1's transfer type -- the TUNING-quality record, one snapshot per zone
 * that survives until the zone is next re-tuned. Set 2 (the per-run FIRING
 * quality history -- mean error/overshoot/IAE/ramp-dwell split) already
 * existed before this pass and lives in profile_executor.c; the two are
 * deliberately not merged, see zones_page.html's "Tuning quality" vs
 * "Firing quality" headings.
 *
 * method/rule are stored as the raw autotune_method_t/autotune_rule_t enum
 * values (pid_autotune.h/autotune_engine.h) rather than typed here, so this
 * header (included from zones_config_json.h, the storage layer) never has
 * to depend on autotune_engine.h/pid_autotune.h -- the caller (autotune_
 * engine.c) casts its own enums to uint8_t, the same convention zone_cfg_t::
 * control_mode already uses for zone_control_mode_t.
 *
 * See zone_cfg_t::tuning_valid's own doc comment (zones_config_json.h) for
 * the full field-by-field provenance and the "valid gates everything else"
 * convention every getter/setter of this type must respect. */
typedef struct {
    bool  valid;                      /* master gate -- false means every other field is unknown */
    uint8_t method;                   /* autotune_method_t raw value: 0=STEP, 1=RELAY */
    uint8_t rule;                     /* autotune_rule_t raw value */
    bool  settled;                    /* fopdt_model_t::settled */
    bool  extrapolation_converged;    /* fopdt_model_t::extrapolation_converged */
    bool  tau_consistent;             /* fopdt_model_t::tau_consistent_with_gain */
    float baseline_c;                 /* fopdt_model_t::baseline_c */
    float step_ambient_c;             /* autotune_engine_status_t::step_ambient_c at fit time */
    float raw_rise_c;                 /* fopdt_model_t::raw_rise_c */
    float rise_inf_c;                 /* fopdt_model_t::rise_inf_c -- (rise_inf_c - raw_rise_c) is the
                                        * extrapolation correction: how much of model_k_dc was
                                        * extrapolated rather than directly measured */
} zone_tuning_quality_t;

/* Getter -- always succeeds for an in-range zone_index regardless of
 * out->valid; false only for a bad zone_index/NULL out, same "false means
 * cannot answer" convention as zones_config_get_model(). The caller MUST
 * check out->valid before trusting any other field -- see
 * zone_tuning_quality_t's own doc comment. */
bool zones_config_get_tuning_quality(uint8_t zone_index, zone_tuning_quality_t *out);

/* Setter -- called by autotune_engine.c's autotune_engine_accept() once a
 * STEP-method run's gains AND plant model are both already persisted (never
 * before either, and never for a RELAY-method run, which measures no FOPDT
 * model to attach a quality record to). q->valid must be true (a caller
 * wanting to CLEAR the record uses zones_config_set_pid(), which already
 * invalidates it -- see that function's own comment; this setter only ever
 * writes a populated record, refusing q==NULL/q->valid==false rather than
 * silently accepting a "set but empty" record that would be indistinguishable
 * from a genuine all-zero fit). Bumps and stores its own tuning_seq (the
 * caller does not supply one) so repeated calls are orderable without a
 * wall-clock timestamp this board has no guaranteed RTC for. Persists
 * immediately, same discipline as every other setter in this file. */
bool zones_config_set_tuning_quality(uint8_t zone_index, const zone_tuning_quality_t *q);

/* zone_cfg_t::fuzzy_strength_pct read-only accessor for the control loop
 * (PID_EXPANSION_PLAN.md Phase 3 wiring, profile_executor.c) -- the one
 * operator-set knob pid_fuzzy_adjust() needs each tick a
 * ZONE_CONTROL_MODE_PID_FUZZY zone runs. 0..100, and 0 is the documented
 * safe default (zero fuzzy adjustment, identical to classic PID). Read-only
 * here: the field is written only via POST /api/zones (zones_http.c's own
 * parse_zone_fields()), never by the control loop. */
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct);

/* Writer for the getter above (PID_EXPANSION_PLAN.md Phase 2/4, 2026-08-30
 * pass: previously read-only). Same bound parse_zone_fields()'s
 * z%u_fuzzy_strength enforces (0..ZONE_FUZZY_STRENGTH_PCT_MAX) -- refused at
 * the door, never clamped, matching every other setter in this file.
 * backup_http.c's import needs this to round-trip the field, the same reason
 * every other setter in this file exists. */
bool zones_config_set_fuzzy_strength_pct(uint8_t zone_index, float pct);

/* zone_cfg_t::coupling_diag_k_dc (PID_EXPANSION_PLAN.md section 3.2 follow-up,
 * ZONES_CFG_VERSION 14->15, 2026-09-02) -- the diagonal cell of the SAME
 * three-run settled-excitation identification that fits coupling_coeff[]'s
 * off-diagonal cross-gains. See zone_cfg_t::coupling_diag_k_dc's own doc
 * comment for the full unit convention and why this is a SEPARATE field from
 * model_k_dc/ff_k_dc (different identification, can legitimately disagree).
 * STORAGE ONLY: zone_coupling_solve.c still substitutes ff_k_dc for the
 * matrix diagonal and is unchanged by this pass -- these accessors exist so
 * the alternative can be persisted ahead of a later, separately reviewed
 * switch. 0 = "not measured", same convention coupling_coeff[] uses. */
bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc);

/* Writer for the getter above. Same bound (0..ZONE_MODEL_K_MAX) validate_
 * zones_cfg()'s coupling_diag_k_dc check enforces -- refused, never clamped,
 * matching every other setter in this file. backup_http.c's import needs
 * this to round-trip the field. */
bool zones_config_set_coupling_diag_k_dc(uint8_t zone_index, float k_dc);

/* zone_cfg_t::coupling_coeff[] (PID_EXPANSION_PLAN.md section 2c's cross-zone
 * feedforward row, widened to a full directed row ZONES_CFG_VERSION 10->11,
 * 2026-08-30 -- a single (coeff, neighbor) pair could not represent the
 * bench-measured matrix: coupling is asymmetric AND every interior zone has
 * multiple neighbors) -- see ZONE_COUPLING_COEFF_MAX's doc comment above and
 * zone_cfg_t::coupling_coeff's own doc comment for what each cell means and
 * its unit convention. out_row must have room for MAX31856_CHANNEL_COUNT
 * floats; out_row[zone_index] (the diagonal) is always 0. */
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT]);

/* ZONES_CFG_VERSION 11->12 siblings of the getter above -- same row shape,
 * same orientation, same diagonal-always-0 rule, for zone_cfg_t::
 * coupling_tau_s[]/coupling_dead_time_s[] (see that field's own doc comment).
 * DATA PLUMBING ONLY: nothing in the control loop reads either yet. */
bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT]);
bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT]);

/* Whole-row setter. Every cell checked before any is written -- same
 * reject-nothing-half-applied discipline as zones_config_set_model().
 * row[zone_index] (the diagonal) MUST be exactly 0; every other cell must be
 * finite and in 0..ZONE_COUPLING_COEFF_MAX. */
bool zones_config_set_coupling(uint8_t zone_index, const float row[MAX31856_CHANNEL_COUNT]);

/* Single-cell setter -- updates ONE neighbor's coefficient without touching
 * the rest of zone_index's row. Exists for autotune_engine.c's finalize_fit():
 * one relay run only measures the running zone's effect on OTHER zones, one
 * cell at a time, and must not wipe out those zones' other already-measured
 * neighbors. Same bounds as the whole-row setter, applied to this one cell;
 * writing the diagonal to exactly 0 is accepted as a no-op (never actually
 * needed in practice, but harmless), any other diagonal value is refused.
 *
 * ZONES_CFG_VERSION 11->12: widened to also take this cell's fitted tau_s/
 * dead_time_s (finalize_fit() fits all three -- K, tau, L -- for every peer
 * in the same pid_autotune_fit_fopdt() call, so this is the one place that
 * ever writes a coupling cell and the natural place to carry the other two
 * through). Same all-or-nothing rule as the coeff bound: tau_s/dead_time_s
 * must each be finite and in 0..ZONE_MODEL_TIME_MAX_S (or exactly 0 on the
 * diagonal, matching coeff), and if either is out of range NOTHING for this
 * cell is written -- not even coeff -- so a cell's three stored numbers can
 * never end up from two different fits. backup_http.c's import is this
 * function's other caller, alongside autotune_engine.c's finalize_fit(). */
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s);

/* zone_cfg_t::settings_source[group] (PID_EXPANSION_PLAN.md section 3.5's
 * "Same as zone N / Custom settings for this zone" UI dropdown, split into
 * SRC_GROUP_COUNT independent per-group bytes by docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs)
 * -- see ZONE_SETTINGS_SOURCE_CUSTOM's doc comment above for the full "0 is
 * a real value here" hazard, and SRC_GROUP_LIMITS et al above for which
 * fields each group covers. `group` must be < SRC_GROUP_COUNT. Getter
 * reports the stored byte verbatim. */
bool zones_config_get_settings_source(uint8_t zone_index, uint8_t group, uint8_t *out_settings_source);

/* Setter for the getter above. Same rule parse_zone_fields()'s
 * z%u_settings_source_<group> enforces: either ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) or a
 * real zone index < MAX31856_CHANNEL_COUNT other than zone_index itself
 * (self-reference is the degenerate inheritance cycle -- refused here, not
 * left for a later pass to unwind). ALSO refused: any settings_source that
 * would close a LONGER cycle through some other zone's already-stored link
 * (2-zone, 3-zone, ...) -- checked against the live stored config for the
 * SAME group only (each group's chain is independent), so this catches
 * everything a single-zone write can create. A whole-page or
 * multi-entry-import write that changes several zones' links AT ONCE, none
 * of which cycles alone against the pre-write config but which cycle
 * together, is NOT caught by this per-call check -- callers doing that (the
 * zones POST handler, backup_http.c's importer) must re-walk the full
 * proposed set themselves before calling this in a commit loop; see
 * zones_config_settings_source_import_has_cycle() for that case. */
bool zones_config_set_settings_source(uint8_t zone_index, uint8_t group, uint8_t settings_source);

/* Commit-loop counterpart to the setter above for a multi-entry import/
 * whole-page write that has ALREADY passed
 * zones_config_settings_source_import_has_cycle() against the full proposed
 * set (see that function's comment). Re-checks only bounds and
 * self-reference -- deliberately OMITS the chain-walk
 * zones_config_set_settings_source() runs against the live config, because
 * during a multi-entry commit loop the live config is PARTIALLY APPLIED and
 * that walk can refuse an intermediate state even though pass 1 already
 * proved the final assembled state is acyclic (e.g. swapping live 0->1,1->0
 * to 0->2,1->CUSTOM one entry at a time). Callers MUST have already run
 * zones_config_settings_source_import_has_cycle() over every candidate in
 * this commit loop -- this function trusts that check instead of repeating
 * it, which is what makes it safe to call in a "pass 2 must not be able to
 * fail" commit loop. Do not call this for a single ad-hoc write outside such
 * a loop -- use zones_config_set_settings_source() for that, which protects
 * itself. */
bool zones_config_set_settings_source_unchecked(uint8_t zone_index, uint8_t group, uint8_t settings_source);

/* Same bounds/self-reference checks as zones_config_set_settings_source_unchecked()
 * above, but does NOT call nvs_save() -- for a caller committing several
 * (zone, group) pairs in one import (backup_import.c's per-zone SRC_GROUP_COUNT
 * fan-out, item 3 of the 5672719 review) that would otherwise trigger one
 * flash write per pair. Caller MUST call nvs_save() itself once after the
 * whole batch of _no_save() calls, or the writes are live-only and do not
 * survive a reboot. */
bool zones_config_set_settings_source_unchecked_no_save(uint8_t zone_index, uint8_t group, uint8_t settings_source);

/* Persists whatever is currently live in s_zones.cfg -- the batch-commit
 * counterpart to the individual setters above, which each save on their own.
 * Callers using the _no_save() setter (or any other no-save mutation) MUST
 * call this exactly once after their whole batch to make the writes survive
 * a reboot. Returns true on success, matching the other setters' convention
 * (they return `nvs_save() == ESP_OK` directly). */
bool zones_config_save_now(void);

/* Cross-entry pass-1 check for a multi-zone import/whole-page write, for ONE
 * group at a time (callers doing several groups run this once per group):
 * `has_override[z]` true means zone z's proposed NEW settings_source for
 * `group` is `override_source[z]`; false means zone z keeps its current
 * LIVE value for that group. Walks every zone's resulting chain (live
 * values everywhere no override is given) and reports whether ANY of them
 * cycles -- catching the case a single zones_config_set_settings_source()
 * call cannot: several zones' links changing in the same import, none of
 * which is a cycle against the old live config alone, but which close one
 * together (e.g. zone 0 -> zone 1 and zone 1 -> zone 0 both newly set in
 * the same import). Returns false (no cycle) with *out_cycle_zone untouched
 * if out_cycle_zone is NULL or no cycle exists; otherwise returns true and,
 * if out_cycle_zone is non-NULL, names one zone that sits on a cycle.
 * Read-only -- never writes to the live config; callers still run their
 * commit loop (calling zones_config_set_settings_source() per entry) only
 * after this returns false, for every group they are importing. */
bool zones_config_settings_source_import_has_cycle(uint8_t group,
                                                    const bool has_override[MAX31856_CHANNEL_COUNT],
                                                    const uint8_t override_source[MAX31856_CHANNEL_COUNT],
                                                    uint8_t *out_cycle_zone);

/* Direction/rate sanity monitor threshold (TODO.md section 6's "reasonable
 * rate ... I will determine later" item) -- degC/minute a zone's actual
 * reading must move, in the commanded direction, over a monitoring window
 * before profile_executor.c treats it as a possibly-detached thermocouple
 * rather than a slow kiln. 0 means "never configured on this zone," which
 * the caller should treat as "use PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN,"
 * not as "disable the check" -- a safety monitor that silently turns itself
 * off because a field was left blank is the wrong default here. */
bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min);

/* Setter for the getter above. Same bound parse_zone_fields()'s z%u_sanity
 * enforces (0..ZONE_SANITY_RATE_MAX_C_PER_MIN) -- 0 is legal here too (it's
 * the documented "never configured" encoding, not a rejection). */
bool zones_config_set_sanity_rate(uint8_t zone_index, float c_per_min);

/* Setter for zones_config_get_max_ramp() above. Same bound
 * parse_zone_fields()'s z%u_ramp enforces
 * (0..ZONE_MAX_RAMP_C_PER_HR_MAX). */
bool zones_config_set_max_ramp(uint8_t zone_index, float c_per_hr);

/* zone_cfg_t::coil_power_w raw accessor (ZONES_CFG_VERSION 24->25). Returns
 * the RAW stored value, 0.0f meaning "not overridden" -- see that field's
 * own ZONE_COIL_POWER_W_MIN/MAX comment in zones_config_json.h. This
 * getter deliberately does NOT resolve the 0 sentinel into an equal share
 * of the whole-kiln nameplate sum: that sum (max_expected_power_w, safety
 * param 0x0319) lives in the safety processor's committed-param cache
 * (safety_cfg_store.c/zone_cfg_committed_f32() in
 * zones_current_sweep_task.c), which this module has no reference to and
 * must not reach into -- the caller who has both numbers
 * (zone_sweep_check_expected_current(), zones_current_sweep_engine.c) does
 * the equal-split resolution itself. Same "false means cannot answer, zero
 * is a real answer" convention as zones_config_get_max_ramp() above. */
bool zones_config_get_coil_power_w(uint8_t zone_index, float *out_power_w);

/* Setter for the getter above. 0.0f is always legal (the documented "not
 * overridden" encoding); a nonzero value must be within
 * [ZONE_COIL_POWER_W_MIN, ZONE_COIL_POWER_W_MAX]. Does not compare against
 * the sum nameplate -- that cross-check happens at the point of use, on the
 * safety processor's committed value, not at write time here (the sum may
 * not have been answered yet, or may change later; a static comparison at
 * write time could go stale). */
bool zones_config_set_coil_power_w(uint8_t zone_index, float power_w);

/* Getter/setter pair for a zone's cal_offset_c -- applied at read time by
 * zones_config_apply_cal() below, but until now there was no way to read the
 * stored offset itself back out (only its already-applied effect on a
 * reading). backup_http.c's export needs the raw stored value to round-trip
 * it, the same reason every other field in this pass got one. Same bound
 * parse_zone_fields()'s z%u_cal enforces. */
bool zones_config_get_cal_offset(uint8_t zone_index, float *out_cal_offset_c);
bool zones_config_set_cal_offset(uint8_t zone_index, float cal_offset_c);

/* TODO.md 6A.1's control modes. OFF: never commands heat (safe default, and
 * what a zone the board supports but the kiln doesn't use should be set to).
 * BANGBANG: relay on/off around setpoint with a fixed hysteresis band, no
 * PID math -- always available, the fallback if tuning is bad. PID: the
 * pid.c loop, rendered onto the relay by heater_output_duty()'s
 * time-proportioning window. PID_FUZZY (PID_EXPANSION_PLAN.md section 2b/
 * Phase 2, added 2026-08-30): same PID loop, but a fixed-rule-table fuzzy
 * layer (pid_fuzzy.c, a later pass) nudges Kp/Ki/Kd around their base
 * autotune-fitted values by up to zone_cfg_t::fuzzy_strength_pct, so one set
 * of tuning numbers holds up across a firing's full temperature range
 * instead of only near where Autotune ran. Appended, never inserted -- this
 * enum is persisted in NVS (see ZONES_CFG_VERSION's own comment on why
 * every enum touched by a stored blob only ever grows at the tail). */
typedef enum {
    ZONE_CONTROL_MODE_OFF = 0,
    ZONE_CONTROL_MODE_BANGBANG = 1,
    ZONE_CONTROL_MODE_PID = 2,
    ZONE_CONTROL_MODE_PID_FUZZY = 3,
} zone_control_mode_t;

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode);

/* Setter for the getter above. Same bound parse_zone_fields()'s z%u_mode
 * enforces (0-3, i.e. <= ZONE_CONTROL_MODE_PID_FUZZY) -- any other numeric
 * value is rejected, matching the POST handler's "out of range (0-3)" error. */
bool zones_config_set_control_mode(uint8_t zone_index, zone_control_mode_t mode);

/* docs/ON_OFF_ZONE_PLAN.md sec 1: "is this zone a heat source at all" --
 * deliberately NOT a zone_control_mode_t value (see that plan section for
 * why overloading mode would be wrong). ZONE_TYPE_HEATER == 0 is the
 * migration default for every existing zone (ZONES_CFG_VERSION 22->23) and
 * the zero-initialized default for a fresh/partial config -- the ordinary,
 * unchanged behaviour every zone on this board has always had. Appended,
 * never inserted -- persisted in NVS, same "enums touched by a stored blob
 * only ever grow at the tail" rule zone_control_mode_t's own comment states. */
typedef enum {
    ZONE_TYPE_HEATER = 0,
    ZONE_TYPE_ON_OFF = 1,
} zone_type_t;

bool zones_config_get_zone_type(uint8_t zone_index, zone_type_t *out_type);

/* Setter for the getter above. Bound to <= ZONE_TYPE_ON_OFF, same discipline
 * as zones_config_set_control_mode() -- an out-of-range value is refused,
 * never clamped. */
bool zones_config_set_zone_type(uint8_t zone_index, zone_type_t type);

/* docs/ON_OFF_ZONE_PLAN.md sec 1's "one rule governs everything" predicate:
 * true iff `zone_index` is a valid, configured ZONE_TYPE_ON_OFF zone. False
 * (never true) for an out-of-range index -- callers that already validate
 * zone_index elsewhere get the same "no such zone, so no such on/off zone"
 * answer as every other zones_config_get_*() failure mode, rather than a
 * separate error path to check. */
bool zone_is_on_off(uint8_t zone_index);

/* docs/ON_OFF_ZONE_PLAN.md sec 2's zone_needs_ceiling(zi) predicate: a HEATER
 * zone always needs a max_temp_c ceiling (the existing, unchanged "0 means
 * uncommissioned, refuse to start" rule); a ZONE_TYPE_ON_OFF zone needs one
 * only if it actually has a thermocouple assigned (thermo_mask != 0) -- an
 * unmeasured channel has no ceiling to apply to. One predicate, one place, so
 * the two call sites (profile_executor_run.c's start refusal and
 * profile_executor_start.c's) cannot drift apart. Returns true (the safer,
 * "needs a ceiling" default) for an out-of-range zone_index, same fail-closed
 * convention as zone_is_on_off() returning false for one -- either predicate
 * mis-evaluating an invalid index must never accidentally relax a safety
 * check. */
bool zone_needs_ceiling(uint8_t zone_index);

/* docs/ON_OFF_ZONE_PLAN.md sec 5's per-zone fail-safe state: 0 = OFF
 * (migration/zero-init default), nonzero = ON. First real reader is
 * on_off_trigger_decide.h's precedence levels 1-3 (control/
 * on_off_trigger_report.c). Fail-closed convention: an out-of-range
 * zone_index returns false and *out_on is left false (OFF), same
 * "never accidentally imply the safer-looking answer is ON" reasoning as
 * every getter above -- a caller that doesn't check the bool return must
 * still land on the safe default. */
bool zones_config_get_failsafe_state(uint8_t zone_index, bool *out_on);

/* Writer for the getter above -- 2026-09-16 backup-round-trip-gap closure
 * (group 1 of the deferred field list: this getter existed with no setter
 * anywhere, so an import had nothing to commit a restored value through).
 * Same "refused, never clamped" discipline as every other setter in this
 * file: on_state is boolean-ish (0/1 only, matching zones_config_json.c's
 * validate_zones_cfg() `z->failsafe_state > 1` check), zone_index must be
 * < the currently configured thermo_count. This is the operator/import
 * writer of the same field on_off_trigger_report.c reads through the
 * getter above -- it does not itself re-evaluate or push anything to the
 * Pico; see on_off_trigger_decide.h for who consumes the new value. */
bool zones_config_set_failsafe_state(uint8_t zone_index, bool on_state);

/* docs/ON_OFF_ZONE_PLAN.md sec 3's temperature hysteresis: stored value 0
 * means "not configured" and the caller substitutes the plan's 2.0 C
 * default -- same "0 substituted with a firmware default" convention
 * thermal_guard_cfg_t's fields already use, so a fresh/migrated zone reads
 * as the documented default rather than a disabled (0-width) band, which
 * would let a boundary-sitting reading chatter the relay from the moment
 * the schema migrates in, before any operator has touched the field.
 * Out-of-range zone_index: returns false, *out_hyst_c left at the 2.0
 * default (not 0) -- same fail-closed reasoning, the safe answer here is
 * the one that CANNOT chatter, not the one that reads as "unset". */
bool zones_config_get_hyst_c(uint8_t zone_index, float *out_hyst_c);

/* Writer for the getter above -- 2026-09-16 backup-round-trip-gap closure
 * (group 2: persisted and readable, but no setter, so a restore could not
 * write it back). 0 is accepted as an explicit "reset to the firmware
 * default", matching validate_zones_cfg()'s own "(0, MIN)-sliver" shape:
 * refused only for a non-zero value outside [ZONE_HYST_C_MIN,
 * ZONE_HYST_C_MAX] or a non-finite one. Never clamped. */
bool zones_config_set_hyst_c(uint8_t zone_index, float hyst_c);

/* docs/ON_OFF_ZONE_PLAN.md sec 3's minimum on/off dwell: stored 0 means
 * "not configured", substituted with the plan's 30 s default. Out-of-range
 * zone_index: returns false, *out_s left at the 30 s default -- same
 * reasoning as zones_config_get_hyst_c() above (the safe default is the
 * one that bounds relay chatter, not zero). */
bool zones_config_get_min_on_s(uint8_t zone_index, uint16_t *out_s);
bool zones_config_get_min_off_s(uint8_t zone_index, uint16_t *out_s);

/* Writers for the two getters above -- same "0 = reset to firmware
 * default" / "(0, MIN)-sliver refused" shape as zones_config_set_hyst_c(),
 * bounded against ZONE_MIN_ON_OFF_S_MIN/MAX. 2026-09-16 backup-round-trip-
 * gap closure, group 2. */
bool zones_config_set_min_on_s(uint8_t zone_index, uint16_t min_on_s);
bool zones_config_set_min_off_s(uint8_t zone_index, uint16_t min_off_s);

/* Guard 5's absolute limits (TODO.md 6A.3). max_temp_c == 0 still means
 * "not set" here, at the storage/getter layer this function lives at --
 * thermal_guard.c's guard 5 continues to read 0 as "no ceiling" and stays a
 * no-op on that zone (this is deliberately unchanged: autotune_engine.c's
 * step-test method relies on exactly this to run an unattended-but-brief,
 * operator-watched probe on a not-yet-commissioned zone -- see its
 * STEP_TEST_GUARD_HEADROOM_C comment).
 *
 * The 2026-08-27 audit ("Guard 5's absolute ceiling is off by default")
 * found this file's OWN doc comment previously claimed max_temp_c == 0
 * followed the same convention as max_ramp_c_per_hr/sanity_rate -- false,
 * and dangerously so for the one field of the three that gates the absolute
 * temperature ceiling. sanity_rate_c_per_min == 0 makes its consumer
 * substitute a firmware default that keeps the check ARMED; max_temp_c == 0
 * makes guard 5 go permanently quiet, the opposite policy wearing an
 * identical "0 = not configured" label. profile_executor.c's
 * profile_executor_run() is what actually closes this gap for a real
 * firing: it now refuses to start whenever an active zone's max_temp_c is 0,
 * matching max_ramp_c_per_hr's existing "0 = uncommissioned, refuse" policy
 * (see that function's guard-5 refusal, added the same date) rather than
 * matching sanity_rate's "0 = substitute a default" policy -- there is no
 * repo-established safe absolute-temperature default to substitute
 * (ZONE_MAX_TEMP_C_MAX below is a 2500C input-sanity bound borrowed from
 * profiles_http.c, not a safe ceiling for an arbitrary kiln). Getters below
 * still faithfully report the stored 0; only the firing-start path treats it
 * as a refusal.
 *
 * min_temp_c has no such special case -- the page's input defaults to -20C
 * (a plausible "colder than any kiln room" floor) so a saved zone always
 * carries a real value. */
bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c);

/* Setter for the getter above. Bundled into ONE call, not two single-field
 * setters, for the same reason zones_config_set_model() bundles its three
 * fields: a rejected call must not leave one of the pair updated and the
 * other stale. Checked: parse_zone_fields() -- the POST /api/zones authority
 * this setter must match -- validates z%u_maxtemp and z%u_mintemp each
 * against their OWN range independently and does NOT compare them to each
 * other (no "max_temp_c >= min_temp_c" check exists anywhere in that path,
 * confirmed by reading the whole handler); an operator can already save an
 * inverted pair through the web page today. This setter deliberately mirrors
 * that -- same two independent bounds, no invented combination check --
 * rather than making backup import stricter than the page it is meant to
 * match; see this pass's report for the same conclusion applied to
 * heater_window_ms/min_on_ms/min_off_ms below. */
bool zones_config_set_temp_limits(uint8_t zone_index, float max_temp_c, float min_temp_c);

/* TODO.md 6A.9: per-zone heater_output_cfg_t timing, page-configurable.
 * 0 in any of the three outputs means "not configured" -- the caller
 * (profile_executor.c/autotune_engine.c) substitutes its own
 * PROFILE_EXECUTOR_DEFAULT_*_MS, same "false/0 means cannot answer, not
 * answer is zero" convention as every other getter here. */
bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms,
                                 float *out_min_off_ms);

/* Setter for the getter above, bundled for the same "no half-updated group"
 * reason zones_config_set_temp_limits() is. Checked: parse_zone_fields()
 * validates z%u_window/z%u_minon/z%u_minoff each against their OWN
 * independent range (ZONE_HEATER_WINDOW_MS_MAX / ZONE_HEATER_MIN_ON_OFF_MS_MAX)
 * and does NOT check min_on_ms/min_off_ms against window_ms -- no "min_on_ms
 * + min_off_ms <= window_ms" or similar exists in that path. This setter
 * matches that exactly, on purpose (see zones_config_set_temp_limits()'s
 * comment for the same reasoning): the goal is parity with the POST
 * authority, not a stricter gate that would make a backup restore reject a
 * combination the web page itself would happily save. */
bool zones_config_set_heater_cfg(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms);

/* Guard 8's cross-zone plausibility threshold (TODO.md 6A.3/6A.5), in degC.
 * 0 means "not configured", and here that DISABLES the guard rather than
 * selecting a default -- unlike sanity_rate_c_per_min. A usable number
 * depends on how strongly this kiln's zones couple, which is what 6A.5's
 * cross-gain matrix measures; until an operator has one, no threshold is
 * better than a hand-picked one that either nuisance-trips a kiln that
 * genuinely stratifies or is too wide to catch anything. The guard also
 * needs >=2 zones reporting to compare against, so it stays inert on a
 * single-zone kiln whatever this is set to. */
bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c);

/* Setter for the getter above. Same bound parse_zone_fields()'s z%u_xzone
 * enforces (0..ZONE_CROSS_ZONE_DELTA_C_MAX) -- 0 is legal (it's the
 * documented "guard disabled" encoding, not a rejection). */
bool zones_config_set_cross_zone_delta(uint8_t zone_index, float max_delta_c);

/* TODO.md 6A.3's remaining named thresholds (wrong-dir rate/window,
 * off-settle, runaway rate/margin, drift period, sensor debounce count,
 * frozen window) -- one getter, matching zones_config_get_heater_cfg()'s
 * "bundle the related group" precedent rather than eight single-field
 * getters. Same "false/0 means cannot answer, not answer is zero" convention
 * as every getter above, and 0 in an output does NOT disable the
 * corresponding guard -- thermal_guard.c substitutes its own firmware
 * default for a 0, same rule as sanity_rate_c_per_min (the opposite
 * convention to cross_zone_max_delta_c, which 0 genuinely disables). */
bool zones_config_get_guard_thresholds(uint8_t zone_index, float *out_wrong_dir_window_s,
                                       float *out_wrong_dir_rate_c_per_min, float *out_off_settle_s,
                                       float *out_runaway_rate_c_per_min, float *out_runaway_margin_c,
                                       float *out_drift_period_s, float *out_sensor_fault_debounce_ticks,
                                       float *out_frozen_window_s);

/* The v8 overrides (2026-08-27), split into the three groups their consumers
 * actually read: thermal_guard.c's five, profile_executor.c's four per-zone,
 * and the one global. Every one keeps the "0 = not configured, the consuming
 * module substitutes its own named default" convention the eight guard
 * thresholds above use -- these getters report the stored value verbatim and
 * never substitute, so the default stays documented in exactly one place (the
 * module that owns it). Return false on a null out-pointer or an out-of-range
 * zone; the per-zone pair are gated on thermo_count, the global one is not.
 *
 * SIGNATURE UNCHANGED by ZONES_CFG_VERSION 9 (2026-08-27, timing profiles):
 * these two still take a zone_index and hand back nine plain floats -- neither
 * caller (profile_executor.c, thermal_guard.c indirectly through it) needed to
 * change at all. What changed is only WHERE zones_http.c reads the nine values
 * from internally: zone_index now resolves to zone_cfg_t::timing_profile, then
 * to that slot in zones_cfg_t::timing_profiles[], instead of to nine fields
 * that used to live directly on zone_cfg_t. A caller asking "what are this
 * zone's timing thresholds" gets the same answer either way, whether three
 * zones share one profile or each has its own. */
bool zones_config_get_guard_extra(uint8_t zone_index, float *out_progress_duty_min,
                                  float *out_progress_window_s, float *out_drift_hysteresis_c,
                                  float *out_frozen_eps_c, float *out_cross_zone_period_s);
bool zones_config_get_executor_thresholds(uint8_t zone_index, float *out_bangbang_hysteresis_c,
                                          float *out_cooling_limited_margin_c,
                                          float *out_cooling_limited_hold_s,
                                          float *out_ramp_lock_band_c);
bool zones_config_get_pc_link_abort_silence_ms(float *out_ms);

/* ---- Timing-profile bundle accessors (2026-09-16 backup-round-trip-gap
 * closure, group 3): zones_config_get_guard_extra()/get_executor_thresholds()
 * above only ever hand back the RESOLVED nine values for a given zone --
 * there was no accessor anywhere that could read or write zone_cfg_t::
 * timing_profile (which profile a zone POINTS AT) or a
 * zone_timing_profile_t bundle itself (its name and nine numbers), so an
 * import had no surface to restore either through. zone_timing_profile_t
 * cannot be named in this header (it is declared in zones_config_json.h,
 * which #includes THIS header, not the other way around -- a forward
 * declaration would still leave callers unable to touch its fields without
 * that header), so this pair follows zones_config_get_guard_thresholds()'s
 * own precedent of spelling the struct out as plain scalar out-params
 * instead of adding a circular include. */

/* Which zones_cfg_t::timing_profiles[] slot this zone currently uses.
 * Same "false = cannot answer" convention as every getter above. */
bool zones_config_get_timing_profile_index(uint8_t zone_index, uint8_t *out_index);

/* Writer for the getter above -- bounded against the CURRENT
 * timing_profile_count (same rule validate_zones_cfg() enforces on a
 * submitted zone_cfg_t::timing_profile: it must reference a profile slot
 * that actually exists). Refused, never clamped. */
bool zones_config_set_timing_profile_index(uint8_t zone_index, uint8_t index);

/* How many of zones_cfg_t::timing_profiles[]'s MAX31856_CHANNEL_COUNT slots
 * are currently meaningful. Never 0 on a config zones_config_is_valid()
 * reports true for. */
uint8_t zones_config_get_timing_profile_count(void);

/* Read one named timing-profile bundle by its slot index (0-based, must be
 * < zones_config_get_timing_profile_count()) -- NOT resolved through any
 * zone, unlike zones_config_get_guard_extra() above. out_name must have at
 * least TIMING_PROFILE_NAME_MAX_LEN+1 bytes; returns false (leaving every
 * output untouched) for an out-of-range profile_index or a null out_name. */
bool zones_config_get_timing_profile_raw(uint8_t profile_index, char *out_name, size_t name_cap,
                                         float *out_progress_duty_min, float *out_progress_window_s,
                                         float *out_drift_hysteresis_c, float *out_frozen_eps_c,
                                         float *out_cross_zone_period_s, float *out_bangbang_hysteresis_c,
                                         float *out_cooling_limited_margin_c,
                                         float *out_cooling_limited_hold_s, float *out_ramp_lock_band_c);

/* Writer for the getter above. profile_index must be <= the CURRENT
 * timing_profile_count -- writing exactly at the current count grows it by
 * one (matching zones_http_post.c's own "profiles submitted 0..N-1
 * contiguously, count becomes N" convention); writing past that leaves an
 * unreachable gap slot no zone_cfg_t::timing_profile could ever validate
 * against, so it is refused rather than silently accepted. Every numeric
 * field is bounded exactly as validate_zones_cfg()'s timing-profile loop
 * enforces (ZONE_GUARD_DUTY_MAX/ZONE_GUARD_TIME_S_MAX/
 * ZONE_GUARD_MARGIN_C_MAX/ZONE_GUARD_EPS_C_MAX); name longer than
 * TIMING_PROFILE_NAME_MAX_LEN is refused, same convention as
 * zones_config_set_name(). Refused (nothing written) for any numeric field
 * out of range, a null/oversized name, or profile_index out of bounds. */
bool zones_config_set_timing_profile_raw(uint8_t profile_index, const char *name,
                                         float progress_duty_min, float progress_window_s,
                                         float drift_hysteresis_c, float frozen_eps_c,
                                         float cross_zone_period_s, float bangbang_hysteresis_c,
                                         float cooling_limited_margin_c, float cooling_limited_hold_s,
                                         float ramp_lock_band_c);

/* Runtime accessor pair for zone_cfg_t::progress_band_c (ZONES_CFG_VERSION
 * 21->22, docs/audits/consumer_without_producer_2026-09-06.md finding 1) --
 * guard 1's arrival band. Declared here (not only zones_config_json.h) so
 * both profile_executor_run.c and autotune_engine.c -- neither of which
 * includes zones_config_json.h directly, both of which #include this header
 * -- see the getter's real definition and full contract at its
 * zones_config_json.h declaration. */
bool zones_config_get_progress_band_c(uint8_t zone_index, float *out_band_c);
bool zones_config_set_progress_band_c(uint8_t zone_index, float band_c);

/* Setter for the getter above, one bundled call matching
 * zones_config_get_guard_thresholds()'s own "bundle the related group"
 * precedent. Each of the 8 fields is checked against its own independent
 * bound (ZONE_GUARD_TIME_S_MAX / ZONE_GUARD_RATE_C_PER_MIN_MAX /
 * ZONE_GUARD_MARGIN_C_MAX / ZONE_GUARD_DEBOUNCE_TICKS_MAX, matching which
 * ceiling parse_zone_fields() applies to each) -- rejecting any one field
 * writes none of the eight, same discipline as zones_config_set_model().
 * No cross-field check between any pair of these 8 exists in
 * parse_zone_fields() either, so none is added here. */
bool zones_config_set_guard_thresholds(uint8_t zone_index, float wrong_dir_window_s,
                                       float wrong_dir_rate_c_per_min, float off_settle_s,
                                       float runaway_rate_c_per_min, float runaway_margin_c,
                                       float drift_period_s, float sensor_fault_debounce_ticks,
                                       float frozen_window_s);

/* Monotonic counter, incremented every time the stored zone config
 * changes -- TODO.md 6A.7's "config reload while running". A consumer
 * (profile_executor.c) caches the value it last read its zone settings at
 * and re-reads them when this differs, instead of re-querying every getter
 * on every control tick or, worse, latching config once at run start and
 * silently ignoring an operator's edit for the rest of a multi-hour firing.
 *
 * Starts at 1, never 0: the natural way to write such a consumer is to
 * leave its cached generation zero-initialized, so 0 must be a value this
 * never returns if "never loaded anything yet" is to reliably compare
 * different from the current config. Unlike the getters above there is no
 * "cannot answer" case -- there is always a current config, even if it is
 * the all-zeros unconfigured one, and the caller only ever compares this
 * value against its own previous copy, never interprets its magnitude.
 *
 * Bumped only for changes that reached the in-RAM struct: a POST rejected
 * by validation never touched it, so it is not a config change. An NVS
 * write that fails after the in-RAM struct was updated IS one -- the live
 * config differs from what the consumer cached regardless of whether it
 * will survive a reboot. Wraparound is a non-issue in practice (2^32
 * operator edits) and harmless in principle, since != is the only test. */
uint32_t zones_config_generation(void);

/* Applies this zone's cal_offset_c to a raw reading -- the "applied in
 * firmware" half of TODO.md section 3's calibration item, at last wired to
 * an actual consumer. Returns raw_c unchanged if zone_index is out of range
 * or raw_c is NaN (NaN + anything is still NaN, but this makes the
 * "unconfigured zone / invalid reading -> pass through" behavior explicit
 * rather than relying on float semantics).
 *
 * SCOPE NOTE: dashboard_http.c's display, profile_executor.c/
 * autotune_engine.c's control math (TODO.md 6A.2/6A.3), readiness_http.c's
 * wizard check, and now uart_bridge.c's THERMO READ payload (2026-08-13,
 * closing the one documented holdout) all call this. A single
 * calibration-provider hook on MAX31856BusClass itself, mirroring the
 * existing drdy_provider pattern, remains the theoretically cleaner
 * lowest-common-point fix, but with every actual consumer now calling
 * through this function there is no live inconsistency left to motivate it
 * -- revisit only if a new consumer reads MAX31856_read() directly instead
 * of going through here. */
float zones_config_apply_cal(uint8_t zone_index, float raw_c);

/* ---- Whole-config export/import for kiln_cfg_store.c ---------------------
 *
 * TODO.md owner-report (2026-08-21 follow-up): "save kiln profiles with
 * different relay/thermocouple/PID configs ... survive a programming cycle,
 * allow creating a config from an existing one." Named KILN CONFIG
 * everywhere (never "profile" -- that word already means a firing schedule
 * in this codebase, profiles_http.c/profile_executor.c). A kiln config is a
 * named, saved copy of the WHOLE zones_cfg_t blob below -- every field this
 * module owns for every channel, plus safety_tc_type -- so kiln_cfg_store.c
 * never needs to know this struct's layout, only that it is an opaque blob
 * of at most ZONES_CONFIG_BLOB_MAX_SIZE bytes that this module alone knows
 * how to interpret, version, migrate, and validate.
 *
 * ZONES_CONFIG_BLOB_MAX_SIZE is a fixed compile-time ceiling kiln_cfg_store.c
 * sizes its own fixed-size storage array against; zones_config_blob_size()
 * (below) is the actual, possibly-smaller runtime size of THIS firmware's
 * zones_cfg_t, used for the export/save path. A future field added to
 * zone_cfg_t/zones_cfg_t that pushes sizeof(zones_cfg_t) past this ceiling
 * fails the _Static_assert in zones_http.c at compile time -- widen this
 * macro then, which also means every kiln_cfg_store entry already on a
 * board's flash keeps its old (smaller) blob size until re-saved, exactly
 * like ZONES_CFG_VERSION's own "grows, never shrinks" migration discipline. */
/* 512 -> 640 (2026-08-30, ZONES_CFG_VERSION 9->10, PID_EXPANSION_PLAN.md
 * Phase 2): zones_cfg_t was already sitting at ~500/512 bytes before this
 * bump (see zones_http.c's zone_normals_cfg_t comment for that measurement);
 * three new floats plus one uint8_t per zone_cfg_t element (fuzzy_strength_pct/
 * coupling_coeff/coupling_neighbor_zone/settings_source) adds ~13 bytes per
 * zone across MAX31856_CHANNEL_COUNT zones, which does not fit in the old
 * ceiling. Existing kiln_cfg_store.c entries already on a board's flash keep
 * their old (smaller) blob size until re-saved -- same discipline as every
 * ZONES_CFG_VERSION migration; see zones_config_blob_size()'s own comment. */
/* 640 -> 768 (2026-09-01, ZONES_CFG_VERSION 12->13, the tuning-quality
 * record -- see zone_cfg_t::tuning_valid's own doc comment): six new
 * uint8_t flags/enums plus four new floats plus one uint32_t sequence
 * counter per zone_cfg_t element adds ~30 bytes per zone across
 * MAX31856_CHANNEL_COUNT (3) zones, ~90 bytes total -- comfortably inside
 * this bump's 128 bytes of headroom. Existing kiln_cfg_store.c entries
 * already on a board's flash keep their old (smaller) blob size until
 * re-saved, same discipline as every prior ZONES_CFG_VERSION migration. */
/* 768 -> 896 (2026-09-04, ZONES_CFG_VERSION 18->19, PID_EXPANSION_PLAN.md
 * sec 3.6g: pid_fuzzy.c's two membership-band widths promoted to per-zone
 * config): two new floats (error_band_c, rate_band_c_per_s) per zone_cfg_t
 * element adds 8 bytes per zone across MAX31856_CHANNEL_COUNT (3) zones, 24
 * bytes total -- comfortably inside this bump's 128 bytes of headroom, same
 * generous-round-number discipline as the 12->13 bump just above. Existing
 * kiln_cfg_store.c entries already on a board's flash keep their old
 * (smaller) blob size until re-saved, same discipline as every prior
 * ZONES_CFG_VERSION migration. */
#define ZONES_CONFIG_BLOB_MAX_SIZE 896

/* Runtime size of the internal zones_cfg_t struct THIS firmware build
 * stores -- what zones_config_export_blob() below actually writes, and the
 * unit kiln_cfg_store.c uses to know how many of a stored blob's bytes are
 * meaningful. Always <= ZONES_CONFIG_BLOB_MAX_SIZE (enforced at compile time
 * in zones_http.c). */
size_t zones_config_blob_size(void);

/* Copies the CURRENT live zones config (s_zones.cfg, exactly what nvs_save()
 * would blob out right now, version byte included) into *out.
 * kiln_cfg_store.c's "save current config under a name" path calls this.
 * Returns false (nothing copied) if out is NULL or out_cap is smaller than
 * zones_config_blob_size(). */
bool zones_config_export_blob(void *out, size_t out_cap);

/* Validates and, only if the WHOLE blob is valid, applies `blob` (len bytes)
 * as the new live zones config -- kiln_cfg_store.c's "apply this saved kiln
 * config" path calls this, AFTER its own caller (kiln_cfg_http.c) has
 * already refused the request via ota_http_check_interlocks() if a firing is
 * running or the heaters are on. This function does NOT check that
 * interlock itself -- it only owns "is this blob internally valid," the same
 * separation of concerns ota_http.h's own header comment describes between
 * ota_http_check_interlocks() and the transfer handlers that call it.
 *
 * Runs the EXACT three-outcome version handling nvs_load_from() uses for a
 * blob read from flash (current version / older version migrated via
 * migrate_zones_cfg_v1_to_current() / newer version REFUSED, since this
 * build does not know that layout and must not guess at it), THEN
 * re-validates every field of the resulting current-version struct against
 * the exact bounds parse_zone_fields()/zones_config_set_*() enforce on a
 * live POST -- a config saved by older firmware, or years ago under looser
 * bounds, must not be trusted just because it was valid once.
 *
 * All-or-nothing: nothing is written to the live config or NVS unless the
 * WHOLE blob -- version handling AND every field's bounds -- passes; a
 * single bad field refuses the entire apply, leaving the running config
 * completely untouched (same discipline as zones_post_handler()'s single
 * commit point). Returns true only on a successful apply (the live config
 * and, best-effort, NVS were updated); on false, reason_out (if non-NULL/
 * non-zero-length) is filled with a specific, human-readable refusal reason,
 * and nothing was changed. */
bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap);

/* ---- Canonical (padding-free) serialization -- H1, docs/audits/
 * kiln_profiles_robustness_2026-09-14.md ------------------------------------
 *
 * zones_config_export_blob() above is a raw memcpy of zones_cfg_t, including
 * whatever bytes the compiler put in its padding. kiln_package_compute_hash()
 * (kiln_package.c) used to hash that raw blob for its ESP half -- fine as
 * long as the only "reconstruction" of zones_cfg_t was another memcpy from a
 * zero-initialized static (always-zero padding), but UNSAFE the moment a
 * package is rebuilt field-by-field from JSON into a not-necessarily-zeroed
 * destination: the recomputed hash would depend on padding bytes the JSON
 * round trip cannot reproduce, refusing an upload of a file the same board
 * produced minutes earlier.
 *
 * zones_config_export_canonical()/zones_config_import_canonical() are the
 * fix: a fixed, declaration-order, field-by-field binary encoding with NO
 * padding, floats emitted as their IEEE-754 bit pattern (-0.0 flushed to
 * +0.0 first) in little-endian, integers little-endian, char arrays copied
 * verbatim. `crc32` is deliberately EXCLUDED from the canonical stream (a
 * whole-struct CRC field cannot canonicalize itself). `zones_cfg_t::crc32`
 * ITSELF is a second instance of the same padding-exposure defect (a
 * whole-struct CRC computed over memcpy'd bytes, field zeroed first) --
 * tracked as a named follow-up in docs/audits/
 * kiln_package_canonical_serializer_2026-09-14.md, NOT fixed by this pair,
 * since today nothing reconstructs a zones_cfg_t from JSON and then
 * recomputes/checks that CRC (zones_config_import_blob() only ever sees a
 * blob that was itself a memcpy of a real board's struct).
 *
 * WHY THIS CANNOT SILENTLY FORGET A FIELD, THE SAME "UN-FORGETTABLE" PROPERTY
 * kiln_package.c's Pico-half table walk already has (see that module's own
 * header comment): the encoder/decoder are generated from ONE X-macro field
 * table per struct (ZONE_TIMING_PROFILE_FIELDS/ZONE_CFG_FIELDS/
 * ZONES_CFG_FIELDS in zones_config_accessors.c), which ALSO generates a
 * "shadow" mirror struct with the identical field list, and a block of
 * _Static_assert()s proving, per field AND for the struct's total size, that
 * the shadow struct's layout is byte-for-byte identical to the real one. A
 * field added to zone_cfg_t/zones_cfg_t/zone_timing_profile_t without adding
 * it to the matching table is NOT silently skipped by the encoder -- it
 * changes the real struct's size and/or the offset of every field after the
 * insertion point, which the shadow-struct comparison catches at COMPILE
 * TIME (a linker-free, always-run static assertion, not a test that must be
 * remembered and invoked) with a message naming the field table to update.
 * See that file's own comment for the one theoretical gap this leaves (a
 * forgotten field whose size exactly consumes an existing alignment-padding
 * gap without moving anything after it) and why it does not occur for any
 * field type this codebase actually uses. */

/* Upper bound on zones_config_export_canonical()'s output for ANY
 * zones_cfg_t this build can hold -- the canonical form carries strictly
 * fewer bytes than the raw struct (no padding, crc32 excluded), so
 * ZONES_CONFIG_BLOB_MAX_SIZE (already the raw-blob ceiling kiln_cfg_store.c
 * sizes its scratch buffers against) is always a safe, if slightly generous,
 * bound -- no separate ceiling macro to keep in sync. */
size_t zones_config_canonical_max_size(void);

/* Encodes `cfg` (any zones_cfg_t instance -- the live s_zones.cfg via
 * zones_config_export_blob()'s sibling accessor, or a candidate built while
 * importing/validating an uploaded package) into `out`. `out_cap` must be >=
 * zones_config_canonical_max_size(); on success `*out_len` is the exact
 * number of bytes written. Never partial -- on any failure (`cfg`/`out`/
 * `out_len` NULL, or `out_cap` too small) nothing is written and `*out_len`
 * is left untouched. */
/* `cfg` is a `const zones_cfg_t *` -- `const void *` here for the same
 * reason zones_config_export_blob() above takes `void *out` rather than
 * `zones_cfg_t *`: this header deliberately does not #include
 * zones_config_json.h (zones_cfg_t's home), so the type is not visible in
 * every translation unit that includes this header. */
bool zones_config_export_canonical(const void *cfg, uint8_t *out, size_t out_cap, size_t *out_len);

/* Inverse of the above. `out` is memset(0) FIRST, unconditionally, before any
 * field is written -- H1's "zero-fill on reconstruction" requirement, so a
 * caller's poisoned or garbage-filled destination buffer can never leave
 * stray bytes behind (there is no padding in `out` this format skips, since
 * the whole struct is populated field-by-field below, but zero-fill-first is
 * the same cheap-insurance convention zones_config_convert.c's own migration
 * paths already use, and it is what makes the round trip byte-identical
 * regardless of what `out` held beforehand).
 *
 * `len` must equal EXACTLY this build's own encoder output length for a
 * fully-populated struct (from zones_config_canonical_max_size() this is not
 * derivable up front since the true encoded length is fixed, not up-to a
 * cap -- callers get the real number back from zones_config_export_canonical()
 * itself). A mismatched length is refused outright (`out` left zeroed,
 * nothing partially decoded) rather than guessed at -- the same "no partial
 * write" discipline as kiln_package_capture_pico_half(). `cfg->crc32` is left
 * at 0; recompute it via the caller's own means if the result will be fed
 * back through a CRC-checking path (zones_config_import_blob() already does
 * this on the raw-blob path and does not use this function). */
bool zones_config_import_canonical(const uint8_t *buf, size_t len, void *out);


/* ---- Task 1: per-zone normal (steady-state) current measurement --------- */

typedef enum {
    ZONE_SWEEP_REFUSE_OK = 0,
    ZONE_SWEEP_REFUSE_ALREADY_RUNNING,
    ZONE_SWEEP_REFUSE_NO_HW,
    ZONE_SWEEP_REFUSE_CONFIG_INVALID,
    ZONE_SWEEP_REFUSE_NO_ZONES,
    ZONE_SWEEP_REFUSE_PROFILE_RUNNING,
    ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING,
    ZONE_SWEEP_REFUSE_LINK_DOWN,
    ZONE_SWEEP_REFUSE_TRIP_LATCHED,
    /* N9 (opus review, 2026-08-28): a relay left ON from the dashboard (or a
     * profile that just ended) before the sweep starts would ride along on
     * whichever zone's energize write happens to share a CT channel with it,
     * inflating that zone's measured "normal" permanently. Refuse to start
     * instead of silently measuring a foreign load. */
    ZONE_SWEEP_REFUSE_RELAYS_ON,
    /* opus review finding (MEDIUM): zone_cfg_committed_ct_topology() reads
     * the safety param cache's 0x031F row and defaults to per_zone (0) when
     * that row has never been set -- but "never set" is indistinguishable
     * from "the cache has never even been fetched from the Pico yet"
     * (safety_cfg_store_fetched_ms_ago() == UINT32_MAX), which is the
     * state of every board's first boot after the v2->v3 store bump. A
     * summed-topology board sweeping under an unfetched cache would
     * silently measure and persist per-zone-shaped normals that are wrong
     * for its actual wiring. Refuse instead of guessing. */
    ZONE_SWEEP_REFUSE_CT_TOPOLOGY_UNKNOWN,
} zone_sweep_refusal_t;

/* Human-readable reason for a zone_sweep_refusal_t -- used by the HTTP
 * handler and safe to call with any enumerator, including
 * ZONE_SWEEP_REFUSE_OK (returns "ok"). */
const char *zone_sweep_refusal_str(zone_sweep_refusal_t r);

typedef enum {
    /* Initial value only, before zones_current_sweep_start() has ever been
     * called this boot -- NOT a state a finished run returns to.
     * zones_current_sweep_start()'s own success path always leaves
     * s_sweep.state at RUNNING immediately, and every terminal state below
     * (DONE/ABORTED/FAILED) is sticky: nothing in zones_http.c ever writes
     * IDLE back into s_sweep.state after a run starts, so "reading a
     * finished run's status returns it to IDLE" (this comment's old wording)
     * never actually happens -- a finished run's result sits at DONE/
     * ABORTED/FAILED until the NEXT zones_current_sweep_start() overwrites it
     * with RUNNING. (LOW, opus review 2026-08-27.) */
    ZONE_SWEEP_IDLE = 0,
    ZONE_SWEEP_RUNNING,
    ZONE_SWEEP_DONE,         /* completed every zone without being aborted or hitting a ceiling */
    ZONE_SWEEP_ABORTED,      /* zones_current_sweep_abort() was called mid-run */
    ZONE_SWEEP_FAILED,       /* a zone's ceiling was hit, or the safety link dropped mid-run */
} zone_sweep_state_t;

/* Attempts to start the sweep as a background task, one zone at a time
 * (structural, not a convention -- see zones_http.c's zone_sweep_task()).
 * Returns ZONE_SWEEP_REFUSE_OK and starts the task, or a specific refusal
 * with NOTHING started -- no relay is ever touched on a refused start.
 * Refuses while: a sweep is already running; hardware was never registered
 * via zones_http_set_hw(); the zones config is not zones_config_is_valid();
 * there are no configured zones; a profile is RUNNING/PAUSED
 * (profile_executor_get_status()); autotune is active
 * (autotune_engine_is_active()); the safety link is down
 * (!safety_link_get_status()->link_up); or a safety trip is latched
 * (SAFETY_LINK_DIAG_STATE_TRIPPED, or the ESP's own fault_asserted output --
 * either one means "do not energize anything right now"); or any relay is
 * already ON per kiln_io_get_relay_shadow() (N9 -- a foreign load left
 * energized would otherwise ride along on the sweep's measurement). */
zone_sweep_refusal_t zones_current_sweep_start(void);

/* B2 (opus review, 2026-08-27): the OTHER half of the interlock --
 * zones_current_sweep_start() already refuses to START a sweep while a
 * profile/autotune run is active, but nothing outside this file used to stop
 * a profile or autotune run from starting WHILE a sweep is active, so
 * starting a sweep and then starting a firing from another tab produced two
 * uncoordinated drivers of the mains-contactor relays at once. Called from
 * profile_executor.c's profile_executor_run(), autotune_engine.c's
 * begin_run_locked(), and ota_http.c's ota_http_check_interlocks() -- each
 * refuses to start/update while this is true, with a reason naming the
 * sweep explicitly, the same way they already name each other
 * (autotune_engine_is_active_on_zone(), ota_http_heat_blocked_by_update()).
 * True for the whole lifetime between a successful zones_current_sweep_start()
 * and the sweep task's own terminal state (DONE/ABORTED/FAILED) -- not just
 * while a relay happens to be energized, since the choke-point-off gap
 * between zones is not a safe window for something else to start driving
 * relays either. */
bool zones_current_sweep_is_active(void);

/* Requests the running sweep stop at its next safe point -- the current
 * zone's relay(s) are dropped (through the same single choke point every
 * other exit path uses, zone_sweep_force_relays_off()) before the task
 * exits. No-op if nothing is running. */
void zones_current_sweep_abort(void);

typedef struct {
    zone_sweep_state_t state;
    uint8_t zone_index;   /* zone currently (or, once finished, last) being measured */
    uint8_t zones_done;   /* zones with a fresh result so far this run */
    uint8_t zones_total;  /* zones this run will attempt (thermo_count, capped to the array) */
    char    reason[64];   /* refusal or failure reason; "" while running/idle/done */
    /* M12: what the last completed run made of the CT-channel -> zone map.
     * ct_map_derived_mask has bit c set for every CT channel that run
     * resolved unambiguously AND wrote to the safety processor;
     * ct_map_reason is "" only when nothing needs saying -- an ambiguous
     * zone, a rejected commit or a down link each put their own sentence
     * here, because a sweep that measured every normal current and still
     * could not map a CT must not read as an unqualified success. */
    uint8_t ct_map_derived_mask;
    char    ct_map_reason[96];
    /* M12b: the same pair for the CT volts-per-amp scale this run
     * calibrated. Separate from ct_map_* because the two derivations fail
     * independently -- a board whose CTs map perfectly but whose operator
     * has not yet answered the mains-voltage/full-output-power questions
     * derives the map and not the scale -- and one shared reason string
     * could only ever report one of the two. k_ct_derived_mask has bit c
     * set for every channel this run calibrated AND confirmed written to
     * the safety processor. */
    uint8_t k_ct_derived_mask;
    char    k_ct_reason[96];
    /* Feature: nameplate current -> S14/S15 arming. bit zi set means zone
     * zi's already-measured normal current (zones_config_get_normal_current(),
     * NOT necessarily measured by THIS run -- see zone_sweep_plan_i_normal())
     * was pushed to and confirmed written on the safety processor's
     * i_normal_a[zi] (0x031A + zi), arming S14 (per-channel over-current) and,
     * in summed-CT topology, S15 (per-zone under-current) for that zone.
     * i_normal_reason is "" only when nothing needs saying, same convention
     * as ct_map_reason/k_ct_reason above. */
    uint8_t i_normal_pushed_mask;
    char    i_normal_reason[96];
    /* opus review finding (MEDIUM), CT_COMMISSIONING_PLAN.md step 3 summed
     * topology: zone_sweep_summed_normal_a() refuses (rather than clamping
     * to a persisted zero) when a zone's shared-channel reading came back
     * lower with the zone on than idle -- a wiring/noise artifact, not a
     * real measurement. Bit zi set here means that zone's normal was NOT
     * written this run and the operator needs to know which zone(s) to
     * re-sweep; zero for every zone in per_zone topology, where this
     * refusal path is never reached. */
    uint8_t summed_unmeasured_mask;
    /* Owner feature (2026-09-10): nameplate-implied expected-current
     * advisory -- see zone_sweep_check_expected_current()'s own doc comment
     * (zones_current_sweep_engine.c) for the full rationale and why this is
     * ESP-side-only and never escalated into a Pico safety trip. Bit zi set
     * means zone zi's measured normal current disagreed with what its
     * nameplate (whole-kiln sum, or a coil_power_w override) implies it
     * should be, by more than the same plausibility band k_ct calibration
     * already uses. nameplate_reason is "" only when nothing needs saying,
     * same convention as the other *_reason fields above. */
    uint8_t nameplate_mismatch_mask;
    char    nameplate_reason[96];
} zone_sweep_status_t;

void zones_current_sweep_get_status(zone_sweep_status_t *out);

/* Task 1's persisted result -- a SEPARATE NVS blob (zone_normals_cfg, see
 * zones_http.c), not a field on zones_cfg_t: zones_cfg_t is already 500 of
 * its 640-byte ZONES_CONFIG_BLOB_MAX_SIZE ceiling (see that macro's own
 * comment), the same reason relay_names_cfg_t got its own blob. Returns
 * false (leaving outputs untouched) for an out-of-range zone_index.
 * *out_measured false means "never measured" -- *out_amps is 0.0f in that
 * case, but callers (zones_ct_mapping_mismatch() below) must branch on
 * *out_measured, never infer "never measured" from a zero amps value, since
 * a real normal current CAN legitimately be very small. */
bool zones_config_get_normal_current(uint8_t zone_index, float *out_amps, bool *out_measured);

/* ---- M12: the CT-channel -> zone mapping the sweep derived ----------------
 * Persisted alongside the measured normals (same NVS blob, v2) so it
 * survives the reboot between running the sweep on the zones page and
 * looking at the commissioning page. *out_derived_mask has bit c set iff
 * out_zone_for_ch[c] is a zone index this board derived from a completed
 * sweep under COMMISSIONING_UX.md sec 1.2's unambiguity condition -- a clear
 * bit means "never derived here", NOT "channel unused", and says nothing
 * about whether the safety processor's own ct_channel_map[c] is set: an
 * operator may always have typed it in by hand instead, and this record has
 * no way to see that. out_zone_for_ch must have room for
 * ZONE_CT_CHANNEL_COUNT bytes; entries whose mask bit is clear are
 * meaningless, never a real zone index. */
void zones_ct_channel_map_derived(uint8_t *out_derived_mask, uint8_t *out_zone_for_ch);

/* ---- M12b: the CT volts-per-amp scale the sweep calibrated ----------------
 * Persisted in the same NVS blob (v3) and for the same reason as the CT map
 * above: the commissioning page has to be able to say DERIVED on a page load
 * that happens long after the sweep ran. *out_derived_mask has bit c set iff
 * out_k_v_per_a[c] is a value this board CALIBRATED from a complete sweep,
 * against the operator's own mains-voltage and full-output-power answers,
 * and then confirmed by reading it back off the safety processor. A clear
 * bit means "never derived here" -- NOT "channel uncalibrated": an operator
 * may always have typed a clamp-meter figure in by hand, and this record has
 * no way to see that. out_k_v_per_a must have room for ZONE_CT_CHANNEL_COUNT
 * floats; entries whose mask bit is clear are meaningless, never a real
 * calibration. This is PROVENANCE only -- the value the safety processor
 * actually uses lives in its own config record, never here. */
void zones_ct_k_v_per_a_derived(uint8_t *out_derived_mask, float *out_k_v_per_a);

/* ---- Task 2: runtime CT-to-zone mapping check ---------------------------- */

/* Pure predicate: does `live_current_a` (this zone's live current right
 * now, summed over its ct_mask channels) plausibly match `normal_current_a`
 * (Task 1's measured normal)? Silent (false) whenever normal_measured is
 * false -- a zone that was never swept has nothing to compare against, and
 * this function must not manufacture a warning from an unmeasured zero (see
 * zones_config_get_normal_current()'s doc comment). A TRUE return is a
 * WARNING for the caller to surface -- catching a CT physically moved to
 * the wrong jack (SaftyFW/docs/CURRENT_SENSE.md §5's commissioning step 2).
 * It is never a trip: that decision belongs to the safety processor, not
 * this file. See zones_http.c for the tolerance band and its rationale. */
bool zones_ct_mapping_mismatch(float normal_current_a, bool normal_measured, float live_current_a);

/* Wiring for the predicate above against LIVE hardware: bit N-1 set (N =
 * zone number, 1-based, same convention as relay_mask/thermo_mask/ct_mask)
 * for every zone that is (a) configured, (b) currently commanded on
 * (kiln_io_get_relay_shadow(), via the io pointer zones_http_set_hw() was
 * given), and (c) mismatching per zones_ct_mapping_mismatch() above, using
 * safety_link_get_status()'s current_a[] summed over the zone's ct_mask.
 * Returns 0 (nothing to warn about) if the io or safety pointer is NULL,
 * the safety link is down, or no zone both qualifies and mismatches. */
uint8_t zones_ct_mapping_warn_mask(void);

/* ---- Task 3: read-only safety-processor wiring display ------------------- */

/* The safety thermocouple's live reading/fault and the safety relay's (K4)
 * live energized state, sourced from safety_link_get_status() through the
 * pointer zones_http_set_hw() was given. Nothing here is settable from this
 * module: reassigning either is not an operator decision (see this
 * struct's use in zones_http.c/zones_page.html), and the one safety-
 * processor value this module DOES own -- safety_tc_type, the configured
 * thermocouple TYPE -- keeps its existing getter/setter pair above
 * unchanged; this struct is the LIVE reading, a different thing.
 *
 * The path this data travels (SaftyFW -> the isolated link -> safety_link.c)
 * is being repaired by another pass as of this comment -- link_up is the
 * one field always trustworthy (it is exactly "did a fresh status frame
 * arrive"); render every other field as an explicit "UNSET"/dash, never a
 * fabricated 0/false, whenever its own *_valid flag (or link_up itself) says
 * not to trust it -- the owner's own ask for this task. Populated even with
 * no hardware registered (io/safety NULL): link_up reads false and every
 * other field its safe zeroed default, identical to what a genuinely-down
 * link reports. */
typedef struct {
    bool    link_up;
    bool    tc_temp_valid;   /* only meaningful if link_up */
    float   tc_temp_c;
    uint8_t tc_fault;        /* MAX31856 SR bits; meaningful only if tc_temp_valid */
    bool    relay_energized; /* SAFETY_FLAG_RELAY; meaningful only if link_up */
    /* ROADMAP.md "Safety TC display audit, 2026-09-05" -- mirrors
     * safety_tc_is_separate_physical_sensor(safety_link.h): true only when
     * the safety processor has confirmed (V3 status frame) that its own
     * thermocouple is NOT reused from a main zone's probe. Consumers must
     * suppress tc_temp_c/tc_fault as a distinct "Thermocouple fault" when
     * this is false -- an unconfirmed or borrowed/both source is not shown
     * as an independent sensor. */
    bool    tc_is_separate_sensor;
} zone_safety_wiring_t;

void zones_get_safety_wiring(zone_safety_wiring_t *out);

/* Live read-back of the Pico's actual configured tc_type, via safety_cfg_
 * store's mirror (param_id 0x0105) -- NOT this module's own deprecated
 * zones_config_get_safety_tc_type() cache above, which can be stale (see
 * this function's doc comment in zones_current_sweep_task.c). Returns false
 * (out_tc_type left at 0) if the Pico has never reported the param yet. */
bool zones_get_safety_pico_tc_type(uint8_t *out_tc_type);

/* Live read-back of the Pico's own S14/S15 arming baseline for channel `zi`,
 * via safety_cfg_store's mirror (param_id 0x031A + zi, "i_normal_a[zi]") --
 * 2026-09-16 config-backup round-trip gap closure. Returns false (out_a left
 * at 0.0f) for zi >= MAX31856_CHANNEL_COUNT or a channel the Pico has never
 * reported a value for (fresh board, or never swept) -- never fabricates a
 * false 0.0 A baseline. See zones_current_sweep_task.c's definition. */
bool zones_get_safety_pico_i_normal_a(uint8_t zi, float *out_a);

#ifdef __cplusplus
}
#endif

#endif // ZONES_CONFIG_ACCESSORS_H
