// config_params.h -- the param_id <-> config_store_record_t field mapping
// docs/COMMISSIONING.md section 2.1 defines and CONFIG_REFERENCE.md sections
// 1-5 name. Pure/host-testable (no RTOS, no pico-sdk, no flash I/O), same
// discipline as config_store.c itself -- src/tasks/link_task.c is the only
// intended caller, using this module to answer three questions:
//
//   - SET_PARAM (0x1C): "stage this (id, type, value) into a RAM record" --
//     config_params_set().
//   - GET_PARAM/GET_CONFIG_PAGE (0x1E/0x1F): "read this id (or every id)
//     back out of a record" -- config_params_get() / config_params_count() /
//     config_params_id_at().
//   - COMMIT_CONFIG (0x1D): "is this staged record, AS A WHOLE, allowed to
//     become the new committed one" -- config_params_validate() /
//     config_params_all_required_set().
//
// Every id in this file's table is permanent once shipped (COMMISSIONING.md
// section 2.1: "a field that is removed leaves its id burned, never
// reused"). `calibration_missing` deliberately has NO entry in the table --
// it is derived by COMMIT_CONFIG (config_params_all_required_set()), never
// set directly by SET_PARAM; see that section's own note on why letting the
// ESP write it directly would make "commissioned" a claim rather than a
// fact.
#ifndef SAFTYFW_CONFIG_PARAMS_H
#define SAFTYFW_CONFIG_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"
#include "kilnlink/kilnlink_param_value.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of ids this module knows about -- the size GET_CONFIG_PAGE's caller
// needs to allocate a scratch array of config_params_id_at() results before
// packing pages (link_task.c's link_task_send_config_page()). A plain
// count/index-based enumeration, not a linked list or callback, so the
// caller can size a fixed stack buffer once and never reallocate.
size_t config_params_count(void);

// Fills `*out_id`/`*out_type` with the `index`-th entry (0-based, <
// config_params_count()) in this module's id table. Returns false (leaving
// both out-params unmodified) if `index` is out of range -- a caller bug,
// not a wire condition, since the table's own size is what bounds every
// legitimate call.
bool config_params_id_at(size_t index, uint16_t *out_id, uint8_t *out_type);

// Reads the field `id` names out of `rec` into `*out_type`/`*out_value`.
// Returns false, leaving both untouched, if `id` is not one of this table's
// known ids -- GET_PARAM's caller (link_task.c) turns that into the wire's
// own `found = 0` reply (COMMISSIONING.md section 2: "an unknown id is
// refused individually and named in the reply, rather than the whole
// transfer failing"), never a guess at what the id might have meant.
bool config_params_get(const config_store_record_t *rec, uint16_t id, uint8_t *out_type,
                        kilnlink_param_value_t *out_value);

// 2026-08-27 audit fix (commissioning-write defect d): whether `id` names a
// field `rec` currently considers SET. For one of the fields_set-gated ids
// (config_store.h's CONFIG_STORE_SET_* bits -- tc_source/borrowed_zone_index/
// tc_placement_mode/abs_max_temp_c/tc_type/ct_channel_map[0..2]/
// max_rate_c_per_min/mains_voltage_v/max_expected_power_w) this reads the
// matching bit out of rec->fields_set (config_store_field_is_set()). Every
// OTHER id in this table has a real compiled-in default and is ALWAYS
// reported set -- CONFIG_REFERENCE.md secs 2-5's threshold fields are never
// "unset" in any sense a caller needs to represent; only the no-safe-default
// fields can be. Returns false (matching config_params_get()'s own contract)
// for an id this table does not recognise.
//
// The one and only intended caller is link_task.c's link_task_send_config_
// page(), which is what closes the defect this exists for: before this
// function existed, that sender emitted every field's raw value with no way
// to say "this one is still unset", and the ESP's cache then marked every
// entry `set = true` unconditionally (KilnFW/App/drivers/safety/safety_cfg_store.c)
// -- an operator-facing page could show abs_max_temp_c "{set:true, value:0}"
// for a field this processor itself considers UNSET, and 0 on that specific
// field means the overtemperature guard NEVER TRIPS.
bool config_params_is_set(const config_store_record_t *rec, uint16_t id);

// Stages `value` (already decoded off the wire, `type` already checked
// against kilnlink_param_value.h's own closed tag set by the SET_PARAM
// codec) into the field `id` names inside `rec`, updating the corresponding
// CONFIG_STORE_SET_* bit in `rec->fields_set` if this id is one of the seven
// no-safe-default fields (config_store.h's own list). Returns false, leaving
// `rec` COMPLETELY untouched, in either of two cases:
//   - `id` is not one of this table's known ids (an unknown/future/removed
//     id -- COMMISSIONING.md section 2's "refused individually" rule);
//   - `type` does not match the field's OWN wire type (e.g. an F32 value
//     arriving for a field this table defines as U16) -- a version-tolerant
//     peer sending the wrong tag for a real id is exactly as untrustworthy
//     as one sending an id that doesn't exist, and must be refused the same
//     way, not coerced.
//   - `value` has the RIGHT wire type but a CONTENT this field can never
//     hold: an enum value outside its named set (tc_source, tc_placement_mode,
//     tc_type, borrowed_type_expected) or documented range
//     (borrowed_zone_index, "0-2"), or a float that is NaN/+-Inf (every F32
//     field, no exceptions -- a NaN threshold makes every comparison a guard
//     runs against it silently false) or negative where the quantity is
//     physically non-negative (abs_max_temp_c, i_present_a). See
//     config_params.c's CHECK_U8_MAX/CHECK_F32_FINITE/CHECK_F32_NONNEG
//     header comment for the full reasoning and which bounds were
//     deliberately left out.
// SET_PARAM only ever stages -- nothing here writes flash; that is
// config_store_write()'s job, called only from COMMIT_CONFIG's handler after
// config_params_validate() passes (see that function below).
bool config_params_set(config_store_record_t *rec, uint16_t id, uint8_t type,
                        kilnlink_param_value_t value);

// Legacy ct_cal[] gain/offset sanity bounds (kilnlink robustness audit
// 2026-10-09, finding L2). ct_amps_cal_apply() computes
// `gain * raw_amps + offset`, clamped at 0, and feeds the result to S14
// (over-current WARN) and S15 (under-current WARN). A calibrated channel with
// gain <= 0 reads 0 A forever, which blinds S14. These are sanity bounds on an
// entry, not commissioning tolerances: gain is a multiplicative correction
// near 1.0 and an order of magnitude above that is already a wrong clamp, and
// the offset band matches the ESP's own CT trim offset band
// (SAFETY_CT_CAL_TRIM_OFFSET_A_MIN/_MAX, safety_cfg_store.h).
//
// gain 0 (and offset 0) stays storable while a channel is NOT calibrated:
// that is the compiled default (config_store_default(),
// ct_amps_cal_uncalibrated_table()), ct_amps_cal_apply() never reads it in
// that state, and kiln_cfg packages re-push it verbatim via SET_PARAM. Only a
// calibrated channel must carry gain > 0.
#define CONFIG_PARAMS_CT_CAL_GAIN_MAX 10.0f

// tc_offset_c magnitude bound (SaftyFW guard review 2026-10-09, F2).
// tc_offset_c is added to the hot-junction reading before EVERY guard sees
// it, so an unbounded offset (e.g. -400) rescales S1's abs_max_temp_c
// bound away entirely. A calibration correction for a K/N/S thermocouple is
// a few degrees (owner's bench value: -4.25 C); +/-50 C leaves an order of
// magnitude of headroom for any real correction while making a blinding
// offset unreachable. Enforced at SET_PARAM time and at load/validate time.
#define CONFIG_PARAMS_TC_OFFSET_ABS_MAX_C 50.0f
#define CONFIG_PARAMS_CT_CAL_OFFSET_ABS_MAX_A 50.0f

// True iff one channel's (calibrated, gain, offset) triple is acceptable to
// store: both floats finite, 0 <= gain <= CONFIG_PARAMS_CT_CAL_GAIN_MAX,
// |offset| <= CONFIG_PARAMS_CT_CAL_OFFSET_ABS_MAX_A, and gain > 0 when
// calibrated. `out_gain_bad` / `out_offset_bad` / `out_rule` are optional and
// say which field and rule failed (gain is reported first).
bool config_params_ct_cal_entry_ok(bool calibrated, float gain, float offset,
                                   bool *out_gain_bad, bool *out_offset_bad,
                                   const char **out_rule);

// Validates the cross-field rules CONFIG_REFERENCE.md states for the staged
// record AS A WHOLE -- COMMISSIONING.md section 2: "Validation happens at
// COMMIT_CONFIG, not at SET_PARAM, because the rules that matter are
// cross-field... a check that cannot be made one field at a time." Pure,
// host-testable: this function never writes flash and never consults ARMED
// state -- that refusal lives inside config_store_write() itself
// (config_store.h's own header comment on config_store_decide_write()), so
// it applies regardless of whether this validation passed.
//
// Returns true iff every rule below passes. On false, `*out_field` and
// `*out_rule` (if non-NULL) are set to short, static, human-readable strings
// naming the offending field and the rule it broke -- COMMISSIONING.md
// section 2's "a commit that fails validation writes nothing and names the
// offending field" -- and `rec` must not be passed to config_store_write().
//
// Rules implemented (CONFIG_REFERENCE.md section 1):
//   - Range/finiteness re-check: every enum, documented finite domain, and
//     float-finiteness/non-negativity bound config_params_set() enforces at
//     SET_PARAM time is re-checked here against the whole record, as a
//     backstop against any `rec` that reached COMMIT_CONFIG WITHOUT going
//     through config_params_set() for every field (a v1-format record
//     migrated forward by config_store_unpack(), for instance). See
//     config_params.c's config_params_set() header comment for which
//     CONFIG_REFERENCE.md line justifies each bound.
//   - tc_placement_mode vs tc_source: when BOTH are staged (fields_set has
//     both CONFIG_STORE_SET_TC_SOURCE and CONFIG_STORE_SET_TC_PLACEMENT_MODE
//     -- an unset field cannot contradict anything), tc_source ==
//     CONFIG_STORE_TC_SOURCE_BORROWED_ZONE requires tc_placement_mode ==
//     CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED. "Forced to CHAMBER_AGREED
//     when tc_source is BORROWED_ZONE" is CONFIG_REFERENCE.md section 1's own
//     wording; "rejected, not silently reconciled" is COMMISSIONING.md
//     section 2's instruction on HOW to enforce it -- this function refuses
//     the whole commit rather than overwriting tc_placement_mode to the
//     value the ESP should have sent.
bool config_params_validate(const config_store_record_t *rec, const char **out_field,
                             const char **out_rule);

// COMMISSIONING.md sec 2/3.1, kilnlink_commit_config_rejected.h -- the coarse
// classification a rejected COMMIT_CONFIG needs to put on the wire.
// config_params_validate()'s out_rule is a free-text C string, fine for
// SaftyFW's own log but too unbounded for a fixed 4-byte wire frame; this
// enum is the wire-sized version of "which kind of rule broke." Every
// RANGE_FAIL site in config_params_validate_ranges() reports RANGE; the
// tc_placement_mode/tc_source cross-field check reports CONTRADICTION.
// link_task.c's COMMIT_CONFIG handler adds ARMED/STORAGE itself, from
// config_store_write()'s own refusal, since validate() never sees that
// failure (it happens after validate() already passed).
typedef enum {
    CONFIG_PARAMS_REJECT_NONE = 0,     // validate() passed; unused on the wire
    CONFIG_PARAMS_REJECT_RANGE,        // one field's value fails its own range/finite check
    CONFIG_PARAMS_REJECT_CONTRADICTION, // two staged fields contradict each other
} config_params_reject_reason_t;

// Same contract as config_params_validate() above, plus `out_reason`
// (optional, NULL-safe) carrying the wire-sized classification of *why*.
// config_params_validate() is now a thin wrapper over this with
// out_reason == NULL, so every existing caller (including test_config_
// store.c's) is unaffected.
bool config_params_validate_ex(const config_store_record_t *rec, const char **out_field,
                                const char **out_rule, config_params_reject_reason_t *out_reason);

// The range/finiteness half of config_params_validate_ex() above, exposed on
// its own (config_params.c keeps it `static` no longer) so config_store.c's
// config_store_unpack() can run it at LOAD time too, not only at
// COMMIT_CONFIG. Deliberately NOT the cross-field tc_source/tc_placement_mode
// contradiction check -- that rule is about what an OPERATOR staged in one
// sitting (COMMISSIONING.md section 2), meaningless to re-litigate against
// bytes a totally different build already decided were internally
// consistent; only "is every field's own value inside the range this build
// can safely act on" is a property flash-load-time integrity, not
// commissioning-session intent, needs re-checked. See config_store.c's
// unpack_v2_fields() caller for why a CRC-valid record from a DIFFERENT
// BUILD still needs this: the CRC only proves the bytes were not corrupted
// in flash, never that this build's field semantics (units, enum meanings,
// valid ranges) are the ones whoever/whatever wrote the record intended --
// a record written by firmware with a wider tc_source enum, for instance,
// could be CRC-valid and byte-identical-looking while still handing this
// build a tc_source value it has no defined behaviour for.
bool config_params_validate_ranges(const config_store_record_t *rec, const char **out_field,
                                    const char **out_rule,
                                    config_params_reject_reason_t *out_reason);

// Maps a config_params_validate()/config_params_validate_ex() out_field
// string to its COMMISSIONING.md sec 2.1 wire param_id, for building a
// SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20) reply (link_task.c). Returns
// CONFIG_PARAMS_NO_PARAM_ID (matching kilnlink_commit_config_rejected.h's
// KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID sentinel byte-for-byte -- see
// this file's own static assertion) for any name this table does not cover,
// including NULL and "rec" (config_params_validate()'s own NULL-rec guard,
// which is not a real staged field).
#define CONFIG_PARAMS_NO_PARAM_ID 0xFFFFu
uint16_t config_params_id_for_field_name(const char *name);

// Derives the ct_channel_map "confirmed as a whole" bit (config_store.h's
// CONFIG_STORE_SET_CT_CHANNEL_MAP) from the three per-channel bookkeeping
// bits SET_PARAM populates (CONFIG_STORE_SET_CT_CHANNEL_MAP_0/_1/_2) --
// called by COMMIT_CONFIG's handler (link_task.c) after
// config_params_validate() passes and before config_store_write(). See
// config_store.h's own header comment on these four bits for why the group
// is derived rather than set directly by an individual SET_PARAM: setting
// only two of the three channels and committing must leave the group unset
// (and therefore calibration_missing true, via
// config_params_all_required_set()), never a false "confirmed."
void config_params_finalize_ct_channel_map(config_store_record_t *rec);

// CT_COMMISSIONING_PLAN.md step 3: "i_present_a auto-derives as half the
// smallest zone normal unless set by hand." Called by COMMIT_CONFIG's
// handler (link_task.c) alongside config_params_finalize_ct_channel_map(),
// same timing (after config_params_validate() passes, before config_store_
// write()). No-op whenever rec->i_present_a_manual is true (0x0301's
// SET_PARAM handler set it) or no i_normal_a[] channel has ever been
// confirmed (CONFIG_STORE_SET_I_NORMAL_A_0/_1/_2) -- see the .c file for the
// exact rule.
void config_params_finalize_i_present_a(config_store_record_t *rec);

// Derives the zone_ct_channel "confirmed as a whole" bit (config_store.h's
// CONFIG_STORE_SET_ZONE_CT_CHANNEL) from the three per-zone bookkeeping bits
// SET_PARAM populates (CONFIG_STORE_SET_ZONE_CT_CHANNEL_0/_1/_2) -- same
// call site, same timing and same reasoning as
// config_params_finalize_ct_channel_map() above: two of three zones answered
// must leave the map untrusted (so readers fall back to the ct_topology-
// derived map via config_store_effective_zone_ct_channel()), never
// half-applied. docs/CT_CHANNEL_MASK.md step 2.
void config_params_finalize_zone_ct_channel(config_store_record_t *rec);

// Closes the CT-commissioning finding: a topology flip (0x031F) or a
// per-zone channel remap (0x0320-0x0322) invalidates the CT verdict
// fingerprint on the ESP (ct_verify_store.h) but did NOT invalidate
// i_normal_a[] here on the Pico, which is indexed BY ZONE and keeps feeding
// S14/S15 (safety_guards.c) a "normal current" measured under whatever
// wiring was in effect when it was recorded. Must be called by COMMIT_CONFIG/
// APPLY_CONFIG_VOLATILE's handlers (link_task.c) AFTER config_params_
// finalize_zone_ct_channel() (so `to_write`'s ZONE_CT_CHANNEL group bit is
// already settled) and BEFORE config_params_finalize_i_present_a() (so a
// zone this call invalidates cannot still win that function's "smallest
// confirmed normal" search). `before` is the record actually committed
// before this commit (config_store_get_full_record(), fetched by the caller
// before applying this commit's SET_PARAM changes on top of it) --
// resolving both sides through config_store_effective_zone_ct_channel()
// (the one function every other "which channel does this zone read" reader
// already goes through) is what lets a topology flip and a direct channel
// remap be caught the same way. See the .c file for the exact per-zone
// clear-and-zero rule.
void config_params_finalize_i_normal_a_invalidation(const config_store_record_t *before,
                                                      config_store_record_t *to_write);

// True iff every one of the seven CONFIG_STORE_SET_* bits (config_store.h's
// bitmask -- the no-safe-default fields CONFIG_REFERENCE.md section 1 names)
// is set in `rec->fields_set`. COMMIT_CONFIG's handler uses this to decide
// `calibration_missing`: cleared only when this returns true, left/forced
// true otherwise (COMMISSIONING.md section 4.1: "a bench preset must not
// look commissioned" -- a partial commit is exactly that case).
bool config_params_all_required_set(const config_store_record_t *rec);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CONFIG_PARAMS_H
