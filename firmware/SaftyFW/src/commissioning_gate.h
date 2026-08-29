// commissioning_gate.h -- "has this safety processor actually been
// commissioned, and may it therefore grant heat?"  ROADMAP.md M12, "An
// uncommissioned safety processor refuses heating enable" (the owner's
// answer to that question was an unqualified NO).
//
// Pure, like discrete_pin_policy.c / relay_grace.c / current_presence_
// policy.c: no RTOS, no pico-sdk, no flash, no logging, no I/O. It reads a
// config_store_record_t the caller already has and returns a decision, so
// the decision itself is host-testable (test/test_commissioning_gate.c)
// independently of safety_core.c's task context, exactly the split every
// other refusal decision on the energize path already uses
// (relay_energize_allowed_during_update(), relay_grace.h).
//
// --- What "commissioned" means here ----------------------------------------
//
// Two independent facts must BOTH say yes:
//
//   1. `rec->calibration_missing == false`. This is the persisted verdict
//      COMMIT_CONFIG's handler (link_task.c) wrote at the moment a real
//      commissioning pass succeeded: `to_write.calibration_missing =
//      !config_params_all_required_set(&to_write)`. It is also the one
//      wire-visible "this board is not fully commissioned" signal the
//      codebase already has (Frame B / SAFETY_CMD_DIAG's
//      KILNLINK_DIAG_FLAG_CALIBRATION_MISSING, link_diag_flags.c), which is
//      what makes this refusal explicable to an operator rather than
//      mysterious -- see the operator-surface note below.
//
//   2. `config_params_all_required_set(rec)` recomputed RIGHT NOW from
//      `rec->fields_set`. Not redundant with (1): (1) is a stored byte, (2)
//      is derived from the bits that record which no-safe-default fields
//      were ever explicitly written. They can legitimately disagree in
//      exactly the direction that matters -- a record whose stored flag
//      says "commissioned" but whose fields_set bits do not back that claim
//      up (a future writer that forgot to recompute the flag, a v1->v2
//      migration, a CRC-valid record whose flag byte was garbled). This
//      module refuses heat whenever the two disagree, in either direction,
//      because "the two sources of truth about commissioning do not agree"
//      is not a state in which anyone should be allowed to make a kiln hot.
//
// Note what this deliberately does NOT accept as commissioned: a record
// that merely holds plausible values. config_store.h is explicit that for
// the no-safe-default fields (`abs_max_temp_c`, `tc_source`,
// `tc_placement_mode`, `borrowed_zone_index`, `ct_channel_map`,
// `max_rate_c_per_min`, `mains_voltage_v`, plus `tc_type` since 2026-08-24)
// every representable value is a value someone could have meant, so only
// the CONFIG_STORE_SET_* bit -- an explicit write, via SET_PARAM +
// COMMIT_CONFIG -- distinguishes a commissioned value from a compiled
// default. A defaulted board is uncommissioned here no matter how sensible
// its numbers look. A partial commit is likewise uncommissioned
// (COMMISSIONING.md section 4.1: "a bench preset must not look
// commissioned").
//
// --- Direction -------------------------------------------------------------
//
// ONLY the ON direction is ever refused. De-energizing is unconditionally
// reachable, the same rule the update interlock and the safety_tc_installed
// refusal in safety_core_request_enable() already follow: a board that
// correctly reports itself unfit to grant heat must still be able to drop
// the relay on command.
//
// --- How the operator finds out --------------------------------------------
//
// No new fault source, trip code or wire field is added by this pass, on
// purpose. The channel already exists end to end: this module's own
// verdict is the same `calibration_missing` that Frame B already reports,
// which KilnFW turns into `commissioned:false` on
// /safety/commissioning (safety_cfg_http.c) and into the FAIL of the
// "Safety processor commissioned" readiness item (readiness_http.c) -- the
// item a firing start is already blocked on. SaftyFW additionally logs the
// refusal ("request_enable: refused, not commissioned", link_task.c's
// existing accepted/refused log line plus safety_core's own). Inventing a
// second, parallel signal for a condition the wire already reports would
// give an operator two things to reconcile instead of one.
#ifndef SAFTYFW_COMMISSIONING_GATE_H
#define SAFTYFW_COMMISSIONING_GATE_H

#include <stdbool.h>

#include "config_store.h" // config_store_record_t

#ifdef __cplusplus
extern "C" {
#endif

// True iff `rec` describes a board that has actually been commissioned --
// both conditions above. NULL is NOT commissioned (a caller that could not
// produce a record must not get heat out of that failure).
bool commissioning_gate_is_commissioned(const config_store_record_t *rec);

// The energize-path decision. `enable` is the direction being requested.
//
//   enable == false -> always true (de-energizing is never gated).
//   enable == true  -> commissioning_gate_is_commissioned(rec).
//
// Same shape and same "only the ON direction" contract as
// relay_energize_allowed_during_update() (relay_grace.h), so
// safety_core_request_enable() reads as one list of interlocks rather than
// a mix of shapes.
bool commissioning_gate_energize_allowed(bool enable, const config_store_record_t *rec);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_COMMISSIONING_GATE_H
