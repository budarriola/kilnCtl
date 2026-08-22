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
