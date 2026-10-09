// current_presence_policy.h -- pure "is current flowing right now" decision,
// split out of current_sense.c the same way max31856_tc_type_policy.h is
// split out of max31856.c (see that header's own comment): this file has no
// pico-sdk/FreeRTOS/hardware includes, so it can be host-tested directly
// (test/test_current_presence_policy.c links this .c file alone) even though
// current_sense.c itself cannot (it calls hardware/adc.h).
//
// Why this exists (2026-08-24, the current-sense-calibration-wiring fix):
// current_sense_set_cal() was never called anywhere in src/, so k_ct_v_per_a
// stayed 0.0f forever and cs_counts_to_amps() (current_sense.c) early-
// returned a hard 0.0f for every channel -- the ONLY producer of
// current_snapshot_t.amps[], which current_any_present() (snapshots.h)
// compares against i_present_a. Net effect: S3/S9/S11/S6b's "is current
// actually flowing" fact could never be true, on ANY board, commissioned or
// not -- a fail-OPEN on four guards, one of which (S3, the welded-SSR
// guard) SAFETY_MODEL.md calls the most valuable guard after S1.
//
// Once current_sense_set_cal() is wired (this same commit), the amps-based
// path works correctly whenever k_ct_v_per_a IS commissioned. But k_ct_v_
// per_a has NO safe default (docs/CURRENT_SENSE.md section 5: "depends on
// which CT model is fitted") and docs/CONFIG_REFERENCE.md section 3
// explicitly documents it as affecting *no guard at all* ("Power estimate
// only") -- unlike i_present_a and zero_counts, which the SAME table
// already lists against S3/S4/S9 and which config_store_default() already
// ships with real, usable values (i_present_a = 2.0A, zero_counts = 0,
// documented as a conservative placeholder, not a "no safe default" field).
// So an UNCOMMISSIONED k_ct_v_per_a should never be able to silently
// disable presence detection -- that contradicts what CONFIG_REFERENCE.md
// already promises, and it is fixable: "is there current flowing" does not
// actually require a volts-per-amp scale factor, only a threshold on the
// raw ADC delta.
//
// current_presence_is_flowing() below is the decoupled decision:
//   - k_ct_v_per_a > 0 (commissioned): converts i_present_a (amps) to a
//     counts threshold using the exact same physics formula cs_counts_to_
//     amps() uses (mirrored here deliberately -- see that function's own
//     comment), so a commissioned channel's presence detection is BIT-FOR-
//     BIT the same decision as "amps > i_present_a" always was. No behavior
//     change on a calibrated board.
//   - k_ct_v_per_a <= 0 (NOT commissioned): falls back to
//     CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS, a small fixed counts-
//     domain floor. This does NOT honor the operator's exact i_present_a
//     value -- it CANNOT: converting an amps threshold to a counts
//     threshold needs the CT's V/A ratio, which is exactly the number that
//     is missing. What it gives instead is deliberately loose ("one to two
//     orders of magnitude of slack" is CONFIG_REFERENCE.md's own stated
//     tolerance for i_present_a) and deliberately biased toward DETECTING
//     current rather than missing it.
//
// Safe-direction reasoning for that bias (the fallback only ever runs on an
// uncommissioned k_ct, which should not happen on a real installation once
// CURRENT_SENSE.md section 5's commissioning step has been done -- but must
// still fail in a defined, safe direction if it does):
//   - S3 (safety_guards.c, "any_current_present && !relay_commanded_
//     recently", the welded-SSR guard) and S9 ("K4 de-energized but current
//     still flowing") both need presence to read TRUE when current is
//     really flowing -- a false NEGATIVE here is the dangerous direction
//     for both (a fail-open weld/ineffective-trip goes undetected). A
//     small, sensitive fallback margin biases toward true positives for
//     these two, which is the safe side.
//   - S11 ("heat_commanded", frozen-sensor guard) only uses presence to
//     ARM the check; a false positive there makes S11 watch for a stuck
//     reading a little more eagerly, never a hazard on its own.
//   - S4 ("relay_commanded_continuously && !any_current_present") is the
//     one guard where a sensitive fallback works AGAINST detection: a
//     genuinely dead/open element could still show a few counts of noise
//     above the fallback margin and be misread as "present", masking the
//     exact fault S4 exists to catch. This is an accepted, documented
//     trade: S4 is 🟠 nuisance-risk in CONFIG_REFERENCE.md (a wrong value
//     causes spurious trips, not a missed hazard), while S3/S9 are 🔴
//     dangerous-risk guards whose own false negative this fallback exists
//     to prevent. Biasing toward detection is the correct trade for the
//     more dangerous failure mode; it is not a free lunch for S4.
//
// The proper fix for the fallback branch ever running at all is
// commissioning k_ct_v_per_a (docs/CURRENT_SENSE.md section 5) -- this
// function only decides what happens in the meantime, and does not treat
// "still uncommissioned" as acceptable to ship a real installation with.
#ifndef SAFTYFW_CURRENT_PRESENCE_POLICY_H
#define SAFTYFW_CURRENT_PRESENCE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The fallback counts-domain margin used only when k_ct_v_per_a <= 0.0f.
// Chosen the same way current_sense.c's own CS_CLIP_MARGIN_COUNTS is
// chosen -- a physical voltage translated into 12-bit counts against the
// 3.3V rail -- but at 20 mV rather than CS_CLIP_MARGIN_COUNTS' 50 mV,
// deliberately smaller/more sensitive than that unrelated top-of-range
// margin: this one exists to catch real current early (see the safe-
// direction reasoning above), not to declare a channel unusably saturated.
// 20 mV / 3.3 V * 4096 counts =~ 25 counts.
#define CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS 25u

// Same two physical constants cs_counts_to_amps() (current_sense.c) uses,
// deliberately duplicated here rather than shared via a header both files
// include -- current_sense.c's own file comment already establishes that
// these are the module's documented physical spec (docs/CURRENT_SENSE.md
// section 5), not implementation details private to one file, so a second
// file computing against the same documented constants is not drift risk
// the way sharing arbitrary implementation state would be.
#define CURRENT_PRESENCE_POLICY_ADC_VREF_V       3.3f
#define CURRENT_PRESENCE_POLICY_ADC_FULL_SCALE   4096.0f
#define CURRENT_PRESENCE_POLICY_SQRT2            1.41421356f

// Decides "is channel `counts_avg` (this pass's 16x-oversampled ADC
// reading) presenting a load right now", decoupled from whether
// k_ct_v_per_a has ever been commissioned -- see this header's own comment
// for the full reasoning and the safe-direction trade this makes.
//
// `gain` must already be the CALLER-resolved gain (current_sense.c
// substitutes its own CS_DEFAULT_GAIN whenever the cal field is <= 0.0f,
// exactly as cs_counts_to_amps() does) -- this function does not know about
// that default, to avoid a second place a compiled physical constant could
// drift from current_sense.c's copy.
bool current_presence_is_flowing(uint32_t counts_avg, uint16_t zero_counts, float i_present_a,
                                   float k_ct_v_per_a, float gain);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CURRENT_PRESENCE_POLICY_H
