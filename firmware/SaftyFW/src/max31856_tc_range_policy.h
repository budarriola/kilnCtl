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
// Uncommissioned tc_type -- UPDATED 2026-08-24. config_store's tc_type field
// now DOES have a fields_set-gating bit (CONFIG_STORE_SET_TC_TYPE,
// config_store.h) precisely so this file's check can stop applying a type's
// exact datasheet band to a value nobody ever confirmed. This file therefore
// exposes TWO functions:
//   - max31856_tc_range_is_plausible(): the original exact-type band, for a
//     GENUINELY commissioned tc_type (caller checks config_store_is_tc_type_
//     set() first).
//   - max31856_tc_range_is_plausible_uncommissioned(): a single, fixed band
//     spanning the union of all eight types' ranges (see TC_RANGES in the
//     .c file for the per-type numbers this is the min/max across), used
//     when tc_type has NOT been commissioned. This is a pure garbage floor,
//     not a type-specific check -- it can never single-handedly prove a
//     reading belongs to whatever type the part is actually wired for, only
//     that the number is not obvious decode/SPI-corruption garbage
//     (nowhere close to ANY real thermocouple's range). Asserting anything
//     tighter against an uncommissioned type would be exactly the "confident,
//     plausible, WRONG" precision this check cannot honestly claim -- the
//     same reasoning that made CONFIG_STORE_SET_TC_TYPE necessary in the
//     first place.
// Why this is a real behavioural WIDENING for a never-commissioned board,
// and why that is still the correct, safe choice: before this bit existed,
// an uncommissioned board got type K's band (-200..+1372degC) unconditionally
// (this file's history, and thermo_task.c's old wiring comment). The union
// band below is wider on the hot end (up to +1820degC, type B's ceiling) --
// so a never-commissioned board now ACCEPTS some readings (1373..1820degC)
// it used to reject. That is intentional, not a regression: this check was
// never the primary ceiling for an uncommissioned board (S1's abs_max_temp_c
// -- itself gated off entirely, "0 = not commissioned", until a real
// ceiling is committed -- and the MAX31856's own hardware TCRANGE bit,
// comparing against whatever CR1 the part actually holds, are both still
// live and unaffected by this change) -- this file's band was always a
// SECOND, independent layer over those, and a second layer that is honest
// about how little it actually knows for an uncommissioned board is safer
// than one that quietly asserts a type-specific boundary nobody confirmed.
// See thermo_task.c's wiring comment for how it picks between the two
// functions.
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

// True iff tc_c falls within the union of all eight types' TEMP RANGEs
// (min across B/E/J/K/N/R/S/T's own lower bounds .. max across their upper
// bounds), for use when tc_type has NOT been commissioned
// (config_store_is_tc_type_set() == false) -- see this file's header comment
// for why a per-type band is unjustifiable in that case and this wider,
// type-agnostic floor is used instead. Same NaN handling as
// max31856_tc_range_is_plausible() (false for NaN). Takes no tc_type
// argument: unlike the function above, this check is deliberately the SAME
// regardless of whatever byte config_store currently holds, since that byte
// is, by construction, a guess in this case.
bool max31856_tc_range_is_plausible_uncommissioned(float tc_c);

// --- Part B: CR1.TC TYPE[3:0] readback verification -------------------------
//
// max31856_configure() (max31856.c) writes CR1 with the caller's tc_type but
// never used to read it back to confirm the part actually accepted it -- a
// failed CR1 write (an SPI transient that the OTHER writes in that same
// function's sequence happen to survive) could leave the part decoding
// against a stale/different type while config_store, and this file's
// exact-band check above, both go on believing the type max31856_configure()
// was ASKED to set. That is invisible to max31856_tc_range_is_plausible():
// it is keyed off config_store's belief, not the part's actual register, by
// design (see this file's header comment on why that is a second,
// independent layer over the MAX31856's own TCRANGE bit) -- but a wrong-CR1
// desync defeats BOTH layers at once, since TCRANGE also compares against
// whatever the part actually holds. Reading CR1 back once, right after
// max31856_configure() writes it, closes that gap independently of either
// band check.
//
// This decision logic lives here, not in max31856.c, for the same
// host-testability reason max31856_tc_type_policy.h/max31856_decode.h are
// split out of max31856.c (that file needs real hardware/gpio.h and
// spi_owner.h and cannot be linked into the host test binary) -- and not in
// a new file, per this pass's own file-ownership constraint against
// creating new source files while another change is mid-flight against
// CMakeLists.txt/test/build_host_tests.ps1. It is unrelated to the
// plausibility-band logic above in subject matter, but pure/host-testable
// for the identical reason, and this is the only extension point this pass
// is allowed to add pure logic to.
typedef enum {
    MAX31856_CR1_READBACK_MATCH = 0,   // TC TYPE[3:0] read back equals what was written
    MAX31856_CR1_READBACK_MISMATCH,    // read back a DIFFERENT real/voltage-mode nibble
    MAX31856_CR1_READBACK_DEAD_BUS,    // whole byte 0x00 or 0xFF -- see below
} max31856_cr1_readback_result_t;

// Compares `cr1_readback` (the full byte max31856.c read back from CR1
// immediately after writing it) against `intended_tc_type` (the value that
// was written, 0..MAX31856_TC_TYPE_T -- max31856_configure() already refuses
// anything else before this is ever called, but this function does not
// itself assume that; an out-of-range `intended_tc_type` simply can never
// produce MATCH).
//
// `cr1_readback == 0x00` or `== 0xFF` is classified DEAD_BUS rather than
// MISMATCH, deliberately: this driver always writes AVGSEL = 4 samples
// (max31856.c's fixed CR1 upper nibble, 0x2X) into the same register, so
// neither whole-byte value can ever be what THIS driver itself wrote for
// ANY real tc_type -- both are the classic symptoms spi_owner.c's own
// baudrate-margin comment warns about (MISO stuck low / stuck high, or the
// whole burst shifted a byte position), not "the part is running some
// other real type". Collapsing that into ordinary MISMATCH would still be
// safe (both outcomes end up not-verified, feeding the same S5 path), but
// would mislabel a dead bus as "a wrong-but-real thermocouple type", which
// is a worse story to hand a bench log than the honest one.
max31856_cr1_readback_result_t max31856_cr1_readback_check(uint8_t intended_tc_type,
                                                             uint8_t cr1_readback);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_TC_RANGE_POLICY_H
