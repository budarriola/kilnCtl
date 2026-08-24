// max31856_tc_range_policy.h -- per-tc_type plausibility range for a
// LINEARIZED thermocouple temperature (LTCBH/M/L, decoded by
// max31856_decode_tc()), split out the same way max31856_tc_type_policy.h
// and max31856_decode.h are (see either header's own comment for the
// general pattern): no pico-sdk/FreeRTOS/hardware includes, so this is
// directly host-tested (test/test_max31856_tc_range_policy.c links this .c
// file alone).
//
// TODO.md Phase 3, "Per-type plausibility ranges, driven from the
// configured type -- a range hard-coded to type K misfires on every other
// type." This is that check.
//
// Where the numbers come from (the datasheet, not general thermocouple
// knowledge, per this pass's own instructions): firmware/KilnFW/Datasheets/
// MAX31856.pdf, page 12, "Table 1. Supported Thermocouples and Temperature
// Ranges", the TEMP RANGE column (the HOT-junction/measurement-junction
// range, not the COLD-JUNCTION TEMP RANGE column next to it -- that one
// governs cj_c, S12's concern, not this file's). Verbatim from the table:
//   B: 250degC to 1820degC       (note: NOT symmetric around 0 -- see below)
//   E: -200degC to +1000degC
//   J: -210degC to +1200degC
//   K: -200degC to +1372degC
//   N: -200degC to +1300degC
//   R: -50degC  to +1768degC
//   S: -50degC  to +1768degC
//   T: -200degC to +400degC
// Type B's range genuinely starts at +250degC, not a transcription error --
// type B thermocouples are non-monotonic/ambiguous below roughly 50degC, and
// the MAX31856's own internal LUT (which this board relies on entirely; see
// max31856.h's file header, "port it, do not rewrite it") simply does not
// cover lower than 250degC for that type. A safety installation actually
// commissioned as type B on a cold chamber would read implausible by this
// check below 250degC -- that is the datasheet's own limit, not a bug here.
//
// Why this duplicates, rather than merely relies on, the MAX31856's own
// unmaskable TCRANGE fault bit (SR bit 6, MAX31856_FAULT_TCRANGE, already
// folded into s5_bad_read_now() in safety_guards.c): TCRANGE reports whether
// the reading is outside the range for whatever thermocouple type the PART
// ITSELF currently has loaded in CR1 right now. That is a real, valuable,
// already-wired check, but it answers a different question than this one.
// It cannot catch the specific hazard this pass exists for: config_store's
// commissioned tc_type and the MAX31856's actual CR1 register can disagree
// -- e.g. max31856_configure()'s CR1 write fails on a given boot (an SPI
// transient; main.c logs and continues per docs/ARCHITECTURE.md section 5
// step 6, "failure must not abort boot") and the part is left running
// whatever type it had before (worst case, its power-on default of Type K).
// In that state the part's own TCRANGE bit is comparing against ITS
// (possibly stale/wrong) type, not the operator's commissioned one, and a
// Type-S sensor decoded through a stuck Type-K LUT can land inside K's wide
// range and never set TCRANGE at all -- exactly the "confident, plausible,
// WRONG" failure this codebase's recurring hazard language describes
// (config_store.h, spi_owner.c). This file's function is keyed off
// config_store's belief instead, so it catches that desync case
// independently of what the chip's hardware currently thinks. It is a
// second, independent layer over the existing hardware one, not a
// replacement for it.
//
// Uncommissioned tc_type -- the crux, argued in full where this is wired in
// (thermo_task.c): config_store's tc_type field has NO fields_set-gating bit
// (config_store.h's own doc comment on the `tc_type` field is explicit that
// this is deliberate, pre-existing, and out of scope for this pass to
// change), so there is no way to distinguish "operator commissioned type K"
// from "nobody has ever touched this, it defaulted to K" at this layer or
// any layer below config_store.h. The check below is applied UNCONDITIONALLY
// against whatever config_store_get_tc_type() currently returns, commissioned
// or not, deliberately -- see thermo_task.c's wiring comment for why that is
// the safe choice for the never-commissioned case specifically (short
// version: type K's own datasheet range, -200 to +1372degC, already covers
// every kiln temperature this codebase's own S1 ceiling reasoning discusses,
// so the never-commissioned board's behaviour does not change in practice).
#ifndef SAFTYFW_MAX31856_TC_RANGE_POLICY_H
#define SAFTYFW_MAX31856_TC_RANGE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// True iff tc_c is a finite, real number that falls within the datasheet's
// linearized TEMP RANGE (Table 1, page 12) for thermocouple type `tc_type`
// (one of MAX31856_TC_TYPE_* -- max31856.h), INCLUSIVE at both ends: the
// datasheet's own text (page 15, "Cold-Junction and Thermocouple
// Out-of-Range Detection") says an out-of-range reading is "clamped at the
// limit value", which makes the limit value itself a genuinely reachable,
// meaningful reading for that type, not an excluded boundary.
//
// Returns false (implausible) for:
//   - NaN (a NaN comparison against any bound is false either way, but this
//     is checked explicitly with isnan() so the contract is documented
//     rather than an accidental fallout of IEEE754 compare semantics).
//   - Any tc_type outside 0..7 (MAX31856_TC_TYPE_B..MAX31856_TC_TYPE_T) --
//     there is no datasheet range to check an unrecognised/voltage-mode type
//     against, and max31856_tc_type_policy.h's own reasoning already
//     excludes 0x08-0x0F from ever reaching max31856_configure() on this
//     board, so this is a defensive floor, not an expected path.
bool max31856_tc_range_is_plausible(uint8_t tc_type, float tc_c);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_TC_RANGE_POLICY_H
